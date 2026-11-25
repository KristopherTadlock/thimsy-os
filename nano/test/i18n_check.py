#!/usr/bin/env python3
"""Consistency check for Nano's translation files (ps3xmb/i18n/*.json).

Fails on:
  * a file that is not valid JSON, or not one object of strings;
  * a duplicate key in a file (NanoI18n keeps the first, so a later fix would be ignored);
  * a missing key: one some language translates and another lacks or leaves empty, and, when the
    Nano sources are reachable, a trDyn()/tfmt()/EN_ONLY() literal, a string a uiStrings() list
    names (the button map's), or a BIOS catalogue row's qualifier or note, that a language does not
    translate;
  * a placeholder mismatch: a translation whose printf conversions differ from its key's in
    number, order or type (NanoI18n's formatSafe drops those at load, so the text stays English);
  * an fnText() key (text naming the function button) without exactly one string argument: one %s,
    or %1$s each time;
  * "Anbernic" in a translation file, or in a string literal of the Nano sources other than a log
    line or the device table: the device's button is named by devkeys::functionPrompt().

The files are found next to this test as Soong installs its data (<dir>/ps3xmb/i18n), else in the
source tree above it, else in $NANO_I18N_DIR. The sources are read from the source tree, or from
$ANDROID_BUILD_TOP/frameworks/base/cmds/gammaos-nano.

usage: i18n_check.py [nano_dir] [--verbose]
"""
import collections
import json
import os
import re
import sys
import unittest

EXCL = ('DevCreds', 'screenscraper_dev', '.local.')
NANO_REL = os.path.join('frameworks', 'base', 'cmds', 'gammaos-nano')


def conversions(s, space_flag):
    """The printf conversions of s in order, as NanoI18n.cpp conversions() finds them."""
    out = []
    i, n = 0, len(s)
    while i < n:
        if s[i] != '%':
            i += 1
            continue
        if i + 1 < n and s[i + 1] == '%':
            i += 2
            continue
        j = i + 1
        while j < n and s[j].isdigit():
            j += 1
        if j < n and s[j] == '$':
            j += 1
        else:
            j = i + 1
        while j < n and (s[j] in "-+#0'" or (space_flag and s[j] == ' ')):
            j += 1
        if j < n and s[j] == '*':
            j += 1
        else:
            while j < n and s[j].isdigit():
                j += 1
        if j < n and s[j] == '.':
            j += 1
            if j < n and s[j] == '*':
                j += 1
            else:
                while j < n and s[j].isdigit():
                    j += 1
        while j < n and s[j] in 'hlLqjzt':
            j += 1
        if j >= n or s[j] not in 'diouxXeEfFgGaAcspn':
            i += 1
            continue
        out.append(s[i:j + 1])
        i = j + 1
    return out


def format_safe(key, val):
    k = conversions(key, False)
    if k != conversions(val, False):
        return False
    return not k or conversions(key, True) == conversions(val, True)


# ---- source literals --------------------------------------------------------------------------

ESC = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34, "'": 39, 'a': 7, 'b': 8, 'f': 12,
       'v': 11, '?': 63}
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CHR = re.compile(r"'(?:[^'\\\n]|\\.)*'")


def decode_c_literal(body):
    b = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            b += c.encode('utf-8')
            i += 1
            continue
        e = body[i + 1]
        if e == 'x':
            j = i + 2
            while j < len(body) and body[j] in '0123456789abcdefABCDEF':
                j += 1
            b.append(int(body[i + 2:j], 16) & 0xFF)
            i = j
        elif e in '01234567':
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in '01234567':
                j += 1
            b.append(int(body[i + 1:j], 8) & 0xFF)
            i = j
        elif e == 'u':
            b += chr(int(body[i + 2:i + 6], 16)).encode('utf-8')
            i += 6
        else:
            b.append(ESC.get(e, ord(e)))
            i += 2
    return bytes(b)


def literal_groups(expr):
    """Maximal runs of adjacent string literals in expr (both arms of a ternary are separate)."""
    groups, cur, pos = [], None, 0
    for m in LIT.finditer(expr):
        between = expr[pos:m.start()]
        if cur is not None and between.strip() == '':
            cur += decode_c_literal(m.group(1))
        else:
            if cur is not None:
                groups.append(cur)
            cur = decode_c_literal(m.group(1))
        pos = m.end()
    if cur is not None:
        groups.append(cur)
    return [g.decode('utf-8', 'replace') for g in groups]


def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            e = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, e))
            i = e
        elif src[i] in '"\'':
            m = (LIT if src[i] == '"' else CHR).match(src, i)
            if m:
                out.append(m.group(0))
                i = m.end()
            else:
                out.append(src[i])
                i += 1
        else:
            out.append(src[i])
            i += 1
    return ''.join(out)


def first_arg(src, open_idx):
    """src[open_idx] == '('; the first top-level argument."""
    depth, i = 0, open_idx
    while i < len(src):
        c = src[i]
        if c in '"\'':
            m = (LIT if c == '"' else CHR).match(src, i)
            i = m.end() if m else i + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                return src[open_idx + 1:i]
        elif c == ',' and depth == 1:
            return src[open_idx + 1:i]
        i += 1
    return src[open_idx + 1:]


def source_keys(nano):
    """English keys the sources translate: key -> 'file:line' of one use."""
    keys = {}
    for f in source_files(nano):
        with open(os.path.join(nano, f), encoding='utf-8', errors='replace') as fh:
            src = strip_comments(fh.read())
        pattern = r'\b(trDyn|tfmt|fnText|EN_ONLY)\s*\(' if f == 'NanoMenuStrings.cpp' else r'\b(trDyn|tfmt|fnText)\s*\('
        for m in re.finditer(pattern, src):
            for g in literal_groups(first_arg(src, m.end() - 1)):
                if g.strip():
                    keys.setdefault(g, '%s:%d' % (f, src.count('\n', 0, m.start()) + 1))
        # A module that leaves its text in English for the caller to translate lists every string in
        # a uiStrings() function (NanoButtonMap.cpp): those are keys too.
        for m in re.finditer(r'\buiStrings\(\)\s*\{', src):
            end = src.find('return', m.end())
            for lm in LIT.finditer(src, m.end(), end if end >= 0 else len(src)):
                g = decode_c_literal(lm.group(1)).decode('utf-8', 'replace')
                if g.strip():
                    keys.setdefault(g, '%s:%d' % (f, src.count('\n', 0, lm.start()) + 1))
        # The BIOS catalogue's rows show their qualifier and note through trDyn.
        if f == BIOS_CATALOG:
            for rm in BIOS_ROW.finditer(src):
                for body in rm.group('qual', 'note'):
                    g = decode_c_literal(body).decode('utf-8', 'replace')
                    if g.strip():
                        keys.setdefault(g, '%s:%d' % (f, src.count('\n', 0, rm.start()) + 1))
    return keys


# {"id", SEC_..., listed, "system", "emulators", "qualifier", "note"}
BIOS_CATALOG = 'NanoBiosCatalog.cpp'
_LIT = r'"(?:[^"\\\n]|\\.)*"'
BIOS_ROW = re.compile(r'\{"[a-z0-9_]+", SEC_[A-Z_]+, (?:true|false), %s, %s, "(?P<qual>(?:[^"\\\n]|\\.)*)", '
                      r'"(?P<note>(?:[^"\\\n]|\\.)*)"\}' % (_LIT, _LIT))


def source_files(nano):
    return sorted(f for f in os.listdir(nano)
                  if f.endswith(('.cpp', '.h', '.inc')) and not any(x in f for x in EXCL))


# The settings tree's rows: a name or description with a %s is shown through fnText.
DATA_TREE = 'NanoMenuPS3Data.h'


def fn_text_keys(nano):
    """fnText() keys: key -> 'file:line' of one use."""
    keys = {}
    for f in source_files(nano):
        with open(os.path.join(nano, f), encoding='utf-8', errors='replace') as fh:
            src = strip_comments(fh.read())
        for m in re.finditer(r'\bfnText\s*\(', src):
            for g in literal_groups(first_arg(src, m.end() - 1)):
                keys.setdefault(g, '%s:%d' % (f, src.count('\n', 0, m.start()) + 1))
        if f == DATA_TREE:
            for lm in LIT.finditer(src):
                text = decode_c_literal(lm.group(1)).decode('utf-8', 'replace')
                if '%s' in text:
                    keys.setdefault(text, '%s:%d' % (f, src.count('\n', 0, lm.start()) + 1))
    return keys


BRAND = 'Anbernic'
LOG_CALL = re.compile(r'\b(ALOG[VDIWE]|SLOG[VDIWE]|LOG_ALWAYS_FATAL(?:_IF)?|LOG_FATAL_IF|__android_log_print)\s*\(')
# (file, literal) pairs that may name the brand: the RG Cube's built-in keys (the device table's
# stand-in) and a saved hide id from before the row was renamed.
BRAND_ALLOWED = {
    ('NanoDeviceKeys.cpp', 'Anbernic'),
    ('NanoMenuPS3Menu.cpp', '/Anbernic Button Opens Quick Menu'),
}


def brand_literals(nano):
    """String literals naming the brand outside log calls and BRAND_ALLOWED: 'file:line: literal'."""
    out = []
    for f in source_files(nano):
        with open(os.path.join(nano, f), encoding='utf-8', errors='replace') as fh:
            src = strip_comments(fh.read())
        logs = []
        for m in LOG_CALL.finditer(src):
            depth, i = 0, m.end() - 1   # the whole call: the brand may be in any argument
            while i < len(src):
                c = src[i]
                if c in '"\'':
                    lm = (LIT if c == '"' else CHR).match(src, i)
                    i = lm.end() if lm else i + 1
                    continue
                depth += c == '('
                depth -= c == ')'
                i += 1
                if depth == 0:
                    break
            logs.append((m.start(), i))
        for lm in LIT.finditer(src):
            text = decode_c_literal(lm.group(1)).decode('utf-8', 'replace')
            if BRAND not in text or (f, text) in BRAND_ALLOWED:
                continue
            if any(a <= lm.start() < b for a, b in logs):
                continue
            out.append('%s:%d: %r' % (f, src.count('\n', 0, lm.start()) + 1, text))
    return out


# ---- locating the files -----------------------------------------------------------------------

def candidate_dirs():
    here = [os.path.dirname(os.path.abspath(p)) for p in (sys.argv[0], __file__) if p]
    dirs = []
    for d in here:
        dirs += [d, os.path.dirname(d)]
    if os.environ.get('ANDROID_BUILD_TOP'):
        dirs.append(os.path.join(os.environ['ANDROID_BUILD_TOP'], NANO_REL))
    dirs.append(os.getcwd())
    return dirs


def find_i18n_dir(nano_arg):
    if os.environ.get('NANO_I18N_DIR'):
        return os.environ['NANO_I18N_DIR']
    for d in ([nano_arg] if nano_arg else []) + candidate_dirs():
        p = os.path.join(d, 'ps3xmb', 'i18n')
        if os.path.isdir(p):
            return p
    return None


def find_source_dir(nano_arg):
    for d in ([nano_arg] if nano_arg else []) + candidate_dirs():
        if os.path.isfile(os.path.join(d, 'NanoI18n.cpp')):
            return d
    return None


# ---- the checks -------------------------------------------------------------------------------

NANO_ARG = None
VERBOSE = False


class I18nTables:
    def __init__(self, i18n_dir):
        self.dir = i18n_dir
        self.langs = sorted(x[:-5] for x in os.listdir(i18n_dir) if x.endswith('.json'))
        self.tables, self.dups, self.parse_errors = {}, {}, {}
        for lang in self.langs:
            dups = []

            def hook(pairs, dups=dups):
                seen = set()
                for k, _ in pairs:
                    if k in seen:
                        dups.append(k)
                    seen.add(k)
                return dict(pairs)

            with open(os.path.join(i18n_dir, lang + '.json'), 'rb') as fh:
                raw = fh.read()
            try:
                t = json.loads(raw.decode('utf-8'), object_pairs_hook=hook)
                if not isinstance(t, dict):
                    raise ValueError('top level is not an object')
            except Exception as e:  # pylint: disable=broad-except
                self.parse_errors[lang] = str(e)
                t = {}
            self.tables[lang] = t
            if dups:
                self.dups[lang] = dups


def report(problems, title):
    if not problems:
        return ''
    shown = problems if VERBOSE else problems[:40]
    more = '' if len(shown) == len(problems) else '\n  ... and %d more' % (len(problems) - len(shown))
    return '%s (%d):\n  %s%s' % (title, len(problems), '\n  '.join(shown), more)


class I18nCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        d = find_i18n_dir(NANO_ARG)
        if not d:
            raise unittest.SkipTest('ps3xmb/i18n not found')
        cls.t = I18nTables(d)

    def test_files_present(self):
        self.assertEqual(len(self.t.langs), 21, 'languages: %s' % ' '.join(self.t.langs))

    def test_parse(self):
        probs = ['%s: %s' % kv for kv in sorted(self.t.parse_errors.items())]
        for lang, t in sorted(self.t.tables.items()):
            probs += ['%s: %r is not a string' % (lang, k) for k, v in t.items() if not isinstance(v, str)]
        self.assertFalse(probs, report(probs, 'unreadable files or non-string values'))

    def test_no_duplicate_keys(self):
        probs = ['%s: %r' % (lang, k) for lang, ks in sorted(self.t.dups.items()) for k in ks]
        self.assertFalse(probs, report(probs, 'duplicate keys'))

    def test_no_missing_keys_across_languages(self):
        union = set()
        for t in self.t.tables.values():
            union.update(k for k, v in t.items() if isinstance(v, str) and v)
        probs = []
        for lang in self.t.langs:
            t = self.t.tables[lang]
            probs += ['%s lacks %r' % (lang, k) for k in sorted(union)
                      if not (isinstance(t.get(k), str) and t.get(k))]
        self.assertFalse(probs, report(probs, 'keys translated in one language but not another'))

    def test_source_keys_translated(self):
        nano = find_source_dir(NANO_ARG)
        if not nano:
            self.skipTest('Nano sources not reachable')
        keys = source_keys(nano)
        probs = []
        for k, where in sorted(keys.items()):
            missing = [lang for lang in self.t.langs
                       if not (isinstance(self.t.tables[lang].get(k), str) and self.t.tables[lang].get(k))]
            if missing:
                langs = 'every language' if len(missing) == len(self.t.langs) else ','.join(missing)
                probs.append('%r (%s) missing in %s' % (k, where, langs))
        self.assertFalse(probs, report(probs, 'source strings with no translation'))

    def test_function_name_placeholders(self):
        nano = find_source_dir(NANO_ARG)
        if not nano:
            self.skipTest('Nano sources not reachable')
        probs = []
        for k, where in sorted(fn_text_keys(nano).items()):
            conv = conversions(k, True)
            if not (conv == ['%s'] or (conv and all(c == '%1$s' for c in conv))):
                probs.append('%r (%s): %s' % (k, where, conv))
        self.assertFalse(probs, report(probs, 'fnText keys without one %s (or %1$s throughout)'))

    def test_no_brand_name_in_ui_text(self):
        probs = ['[%s] %r' % (lang, k) for lang in self.t.langs for k, v in self.t.tables[lang].items()
                 if BRAND in k or (isinstance(v, str) and BRAND in v)]
        nano = find_source_dir(NANO_ARG)
        if nano:
            probs += brand_literals(nano)
        self.assertFalse(probs, report(probs, '"%s" in UI text (use devkeys::functionPrompt())' % BRAND))

    def test_placeholders_match(self):
        probs = []
        for lang in self.t.langs:
            for k, v in self.t.tables[lang].items():
                if isinstance(v, str) and v and not format_safe(k, v):
                    probs.append('[%s] %r -> %r: key %s, translation %s'
                                 % (lang, k, v, conversions(k, True), conversions(v, True)))
        self.assertFalse(probs, report(probs, 'translations whose placeholders differ from the key'))


def main():
    global NANO_ARG, VERBOSE
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    VERBOSE = '--verbose' in sys.argv
    NANO_ARG = args[0] if args else None
    argv = [sys.argv[0]] + (['-v'] if VERBOSE else [])
    result = unittest.main(argv=argv, exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == '__main__':
    main()
