// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/PadSpec.h"

#include <linux/input.h>

#include <set>

#include "core/InputCodes.h"
#include "thimsy/atomic_file.h"
#include "thimsy/json.h"

namespace padd {

using thimsy::json::Value;

bool matches(const Match& m, const NodeInfo& node) {
    if (m.name != node.name) return false;
    if (m.phys && *m.phys != node.phys) return false;
    if (m.uniq && *m.uniq != node.uniq) return false;
    if (m.bus >= 0 && m.bus != node.bus) return false;
    if (m.vendor >= 0 && m.vendor != node.vendor) return false;
    if (m.product >= 0 && m.product != node.product) return false;
    return true;
}

Target targetFor(uint16_t code) {
    Target t;
    t.code = code;
    if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) {
        t.kind = TargetKind::Hat;
    } else if ((code >= BTN_MISC && code < KEY_OK) ||
               (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40)) {
        t.kind = TargetKind::PadKey;  // the BTN_ ranges
    } else {
        t.kind = TargetKind::KeysKey;  // a keyboard key
    }
    return t;
}

int PadSpec::sourceIndex(std::string_view id) const {
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

namespace {

// The normalised buttons padd emits (schema "pad_button"), besides KEY_*.
const std::set<std::string>& padButtonNames() {
    static const std::set<std::string> names = {
        "BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST", "BTN_TL", "BTN_TR",
        "BTN_TL2", "BTN_TR2", "BTN_SELECT", "BTN_START", "BTN_MODE", "BTN_THUMBL",
        "BTN_THUMBR", "BTN_DPAD_UP", "BTN_DPAD_DOWN", "BTN_DPAD_LEFT", "BTN_DPAD_RIGHT"};
    return names;
}

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

bool readMatch(const Value& v, Match* m, const std::string& where, std::string* err) {
    if (!v.isObject()) return fail(err, where + ": match is not an object");
    const Value& name = v["name"];
    if (!name.isString() || name.asString().empty()) return fail(err, where + ": match.name is missing");
    m->name = name.asString();
    if (const Value* p = v.find("phys")) {
        if (!p->isString()) return fail(err, where + ": match.phys is not a string");
        m->phys = p->asString();
    }
    if (const Value* u = v.find("uniq")) {
        if (!u->isString()) return fail(err, where + ": match.uniq is not a string");
        m->uniq = u->asString();
    }
    struct {
        const char* key;
        int* field;
    } ids[] = {{"bus", &m->bus}, {"vendor", &m->vendor}, {"product", &m->product}};
    for (auto& id : ids) {
        if (const Value* x = v.find(id.key)) {
            if (!x->isInt() || x->asInt() < 0 || x->asInt() > 0xffff) {
                return fail(err, where + ": match." + id.key + " is not a 16-bit integer");
            }
            *id.field = static_cast<int>(x->asInt());
        }
    }
    return true;
}

bool readRole(const std::string& s, Role* role) {
    if (s == "buttons") *role = Role::Buttons;
    else if (s == "sticks") *role = Role::Sticks;
    else if (s == "buttons+sticks") *role = Role::ButtonsSticks;
    else if (s == "rumble") *role = Role::Rumble;
    else return false;
    return true;
}

bool optInt(const Value& obj, const char* key, std::optional<int32_t>* out, const std::string& where,
            std::string* err) {
    const Value* v = obj.find(key);
    if (!v) return true;
    if (!v->isInt() || v->asInt() < INT32_MIN || v->asInt() > INT32_MAX) {
        return fail(err, where + "." + key + " is not an integer");
    }
    *out = static_cast<int32_t>(v->asInt());
    return true;
}

bool readCalibration(const Value& v, Calibration* cal, const std::string& where, std::string* err) {
    if (v.isNull()) return true;
    if (!v.isObject()) return fail(err, where + " is not an object");
    return optInt(v, "min", &cal->min, where, err) && optInt(v, "max", &cal->max, where, err) &&
           optInt(v, "center", &cal->center, where, err) &&
           optInt(v, "deadzone", &cal->deadzone, where, err) &&
           optInt(v, "fuzz", &cal->fuzz, where, err) && optInt(v, "flat", &cal->flat, where, err);
}

// A pad_button: a normalised BTN_ name or any known KEY_.
int readPadButton(const Value& v) {
    if (!v.isString()) return -1;
    const std::string& s = v.asString();
    if (s.compare(0, 4, "KEY_") == 0) return keyCodeByName(s);
    if (!padButtonNames().count(s)) return -1;
    return keyCodeByName(s);
}

bool roleHasButtons(Role r) { return r == Role::Buttons || r == Role::ButtonsSticks; }
bool roleHasAxes(Role r) { return r == Role::Sticks || r == Role::ButtonsSticks; }

bool readAxis(const PadSpec& spec, const Value& v, int source, bool trigger, const std::string& where,
              AxisMap* out, std::string* err) {
    if (!v.isObject()) return fail(err, where + " is not an object");
    int code = absCodeByName(v["code"].asString());
    if (code < 0) return fail(err, where + ".code: unknown axis '" + v["code"].asString() + "'");
    const std::string& to = v["to"].asString();
    int toCode = absCodeByName(to);
    bool ok = trigger ? (to == "ABS_Z" || to == "ABS_RZ")
                      : (to == "ABS_X" || to == "ABS_Y" || to == "ABS_RX" || to == "ABS_RY");
    if (toCode < 0 || !ok) return fail(err, where + ".to: '" + to + "' is not allowed here");
    out->source = source;
    out->code = static_cast<uint16_t>(code);
    out->to = static_cast<uint16_t>(toCode);
    out->trigger = trigger;
    if (const Value* inv = v.find("invert")) {
        if (!inv->isBool()) return fail(err, where + ".invert is not a boolean");
        out->invert = inv->asBool();
    }
    (void)spec;
    return readCalibration(v["calibration"], &out->cal, where + ".calibration", err);
}

}  // namespace

bool parsePadSpec(const Value& device, PadSpec* out, std::string* err,
                  std::vector<std::string>* warnings) {
    PadSpec spec;
    std::vector<std::string> warn;
    const Value& pad = device["pad"];
    if (!pad.isObject()) return fail(err, "device.json has no pad section");

    const std::string& producer = pad["producer"].asString();
    if (producer == "padd") spec.producer = Producer::Padd;
    else if (producer == "kernel-joypad") spec.producer = Producer::KernelJoypad;
    else return fail(err, "pad.producer: unknown '" + producer + "'");

    if (!pad["sources"].isArray()) return fail(err, "pad.sources is not an array");
    for (size_t i = 0; i < pad["sources"].size(); ++i) {
        const Value& s = pad["sources"][i];
        std::string where = "pad.sources[" + std::to_string(i) + "]";
        if (!s.isObject()) return fail(err, where + " is not an object");
        Source src;
        src.id = s["id"].asString();
        if (src.id.empty()) return fail(err, where + ".id is missing");
        if (spec.sourceIndex(src.id) >= 0) return fail(err, where + ": duplicate id '" + src.id + "'");
        if (!readRole(s["role"].asString(), &src.role)) {
            return fail(err, where + ".role: unknown '" + s["role"].asString() + "'");
        }
        if (!readMatch(s["match"], &src.match, where, err)) return false;
        src.required = s["required"].asBool(true);
        src.grab = s["grab"].asBool(src.role != Role::Rumble);
        src.hide = s["hide"].asBool(true);
        spec.sources.push_back(src);
    }

    int inputSources = 0;
    for (const Source& s : spec.sources) inputSources += s.role != Role::Rumble;
    if (spec.producer == Producer::KernelJoypad && inputSources != 1) {
        return fail(err, "pad: producer kernel-joypad needs exactly one input source, not " +
                             std::to_string(inputSources));
    }

    if (!pad["buttons"].isArray()) return fail(err, "pad.buttons is not an array");
    std::set<std::pair<int, int>> seen;
    for (size_t i = 0; i < pad["buttons"].size(); ++i) {
        const Value& b = pad["buttons"][i];
        std::string where = "pad.buttons[" + std::to_string(i) + "]";
        if (!b.isObject()) return fail(err, where + " is not an object");
        ButtonMap m;
        m.source = spec.sourceIndex(b["source"].asString());
        if (m.source < 0) return fail(err, where + ".source: unknown '" + b["source"].asString() + "'");
        if (!roleHasButtons(spec.sources[m.source].role)) {
            return fail(err, where + ".source: '" + b["source"].asString() + "' has no buttons role");
        }
        int code = keyCodeByName(b["code"].asString());
        if (code < 0) return fail(err, where + ".code: unknown '" + b["code"].asString() + "'");
        int to = readPadButton(b["to"]);
        if (to < 0) return fail(err, where + ".to: not a pad button '" + b["to"].asString() + "'");
        if (!seen.insert({m.source, code}).second) {
            return fail(err, where + ": " + b["source"].asString() + " " + b["code"].asString() +
                                 " is mapped twice");
        }
        m.code = static_cast<uint16_t>(code);
        m.to = targetFor(static_cast<uint16_t>(to));
        m.label = b["label"].asString("");
        spec.buttons.push_back(m);
    }
    spec.passThrough = spec.producer == Producer::KernelJoypad && spec.buttons.empty();

    std::set<uint16_t> axisTargets;
    auto addAxis = [&](const AxisMap& a, const std::string& where) {
        if (!axisTargets.insert(a.to).second) {
            return fail(err, where + ": " + eventCodeName(EV_ABS, a.to) + " is a target twice");
        }
        spec.axes.push_back(a);
        return true;
    };
    const Value& sticks = pad["sticks"];
    if (!sticks.isNull() && !sticks.isArray()) return fail(err, "pad.sticks is not an array");
    for (size_t i = 0; i < sticks.size(); ++i) {
        const Value& st = sticks[i];
        std::string where = "pad.sticks[" + std::to_string(i) + "]";
        int source = spec.sourceIndex(st["source"].asString());
        if (source < 0) return fail(err, where + ".source: unknown '" + st["source"].asString() + "'");
        if (!roleHasAxes(spec.sources[source].role)) {
            return fail(err, where + ".source: '" + st["source"].asString() + "' has no sticks role");
        }
        for (const char* axis : {"x", "y"}) {
            AxisMap a;
            if (!readAxis(spec, st[axis], source, false, where + "." + axis, &a, err)) return false;
            if (!addAxis(a, where + "." + axis)) return false;
        }
    }
    const Value& triggers = pad["triggers"];
    if (!triggers.isNull() && !triggers.isArray()) return fail(err, "pad.triggers is not an array");
    for (size_t i = 0; i < triggers.size(); ++i) {
        const Value& t = triggers[i];
        std::string where = "pad.triggers[" + std::to_string(i) + "]";
        int source = spec.sourceIndex(t["source"].asString());
        if (source < 0) return fail(err, where + ".source: unknown '" + t["source"].asString() + "'");
        AxisMap a;
        if (!readAxis(spec, t, source, true, where, &a, err)) return false;
        if (!addAxis(a, where)) return false;
    }

    if (const Value* r = pad.find("rumble")) {
        spec.rumbleSource = spec.sourceIndex((*r)["source"].asString());
        if (spec.rumbleSource < 0) {
            return fail(err, "pad.rumble.source: unknown '" + (*r)["source"].asString() + "'");
        }
    }
    spec.faceButtons = static_cast<int>(pad["face_buttons"].asInt(0));
    spec.faceLayout = pad["face_layout"].asString("");

    if (const Value* h = pad.find("hotkey")) {
        spec.hotkey = readPadButton((*h)["button"]);
        if (spec.hotkey < 0) return fail(err, "pad.hotkey.button: not a pad button");
        const Value& combo = (*h)["quit_combo"];
        for (size_t i = 0; i < combo.size(); ++i) {
            int c = readPadButton(combo[i]);
            if (c < 0) return fail(err, "pad.hotkey.quit_combo[" + std::to_string(i) + "]: not a pad button");
            spec.quitCombo.push_back(static_cast<uint16_t>(c));
        }
        if (!spec.passThrough) {
            bool mapped = false;
            for (const ButtonMap& b : spec.buttons) mapped |= b.to.code == spec.hotkey;
            if (!mapped) {
                warn.push_back("pad.hotkey.button " + eventCodeName(EV_KEY, spec.hotkey) +
                               " is not the target of any button");
            }
        }
    }

    for (size_t i = 0; i < spec.sources.size(); ++i) {
        const Source& s = spec.sources[i];
        if (s.role == Role::Rumble && static_cast<int>(i) != spec.rumbleSource) {
            warn.push_back("source '" + s.id + "' has role rumble but pad.rumble does not name it");
        }
        if (s.role != Role::Rumble && !spec.passThrough) {
            bool used = false;
            for (const ButtonMap& b : spec.buttons) used |= b.source == static_cast<int>(i);
            for (const AxisMap& a : spec.axes) used |= a.source == static_cast<int>(i);
            if (!used) warn.push_back("source '" + s.id + "' has no mapped buttons or axes");
        }
    }

    *out = std::move(spec);
    if (warnings) *warnings = std::move(warn);
    return true;
}

bool loadPadSpec(const std::string& path, PadSpec* out, std::string* err,
                 std::vector<std::string>* warnings) {
    std::string text;
    if (!thimsy::readFile(path, &text)) return fail(err, path + ": can't read");
    Value doc;
    std::string perr;
    if (!thimsy::json::parse(text, &doc, &perr)) return fail(err, path + ": " + perr);
    std::string serr;
    if (!parsePadSpec(doc, out, &serr, warnings)) return fail(err, path + ": " + serr);
    return true;
}

std::string checkCaps(const PadSpec& spec, int source, const SourceCaps& caps) {
    if (source < 0 || source >= static_cast<int>(spec.sources.size())) return "no such source";
    const Source& s = spec.sources[source];
    // An input source that is also pad.rumble's source may lack FF: then there is no rumble.
    if (s.role == Role::Rumble && !caps.ff.count(FF_RUMBLE)) return "no FF_RUMBLE";
    if (roleHasButtons(s.role)) {
        if (caps.keys.empty()) return "no EV_KEY codes";
        if (!spec.passThrough) {
            bool any = false;
            for (const ButtonMap& b : spec.buttons) {
                if (b.source == source && caps.keys.count(b.code)) any = true;
            }
            if (!any) return "none of its mapped buttons";
        }
    }
    if (roleHasAxes(s.role)) {
        for (const AxisMap& a : spec.axes) {
            if (a.source == source && !caps.abs.count(a.code)) {
                return "no " + eventCodeName(EV_ABS, a.code);
            }
        }
    }
    return "";
}

int pickSource(const PadSpec& spec, const NodeInfo& node, const SourceCaps& caps,
               const std::vector<bool>& taken, std::string* why) {
    std::string reasons;
    for (size_t i = 0; i < spec.sources.size(); ++i) {
        if (i < taken.size() && taken[i]) continue;
        if (!matches(spec.sources[i].match, node)) continue;
        std::string r = checkCaps(spec, static_cast<int>(i), caps);
        if (r.empty()) return static_cast<int>(i);
        if (!reasons.empty()) reasons += "; ";
        reasons += spec.sources[i].id + ": " + r;
    }
    if (why) *why = reasons.empty() ? "matches no source" : reasons;
    return -1;
}

}  // namespace padd
