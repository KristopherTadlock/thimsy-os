"""projects/ROCKNIX/config.xml: families, sub-images, DTB names, fdt and overlays.

Real shape (next, 0f4806b):

    <rocknix>
      <RK3566 dtb_prefix="rockchip">
        <Generic mkimage_options="dtb,extlinux,uboot" fdt="device_trees" fdt_type="fdtdir">
          <file>rk3566-anbernic-rg-arc-d</file> ...
        </Generic>
        <Specific ... fdt="device_trees/rk3566-powkiddy-x55.dtb" fdt_type="fdt">...</Specific>
      </RK3566>
      <RK3399 mkimage_options="..." fdt="..." fdt_type="fdt" dtb_prefix="rockchip">
        <file>rk3399-anbernic-rg552</file>            (no sub-image: files on the family)
      </RK3399>

A sub-image inherits the family's attributes and may add its own (fdt_overlays on H700's DDR3).
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from collections import OrderedDict

from .rocknix import ImportError_

IMAGE_ATTRS = ("mkimage_options", "fdt", "fdt_type", "fdt_overlays")


class Image:
    def __init__(self, name: str, attrs: dict, files: list):
        self.name = name          # "" when the files sit directly on the family
        self.attrs = attrs
        self.files = files

    def to_json(self) -> dict:
        d = OrderedDict()
        d["name"] = self.name
        for k in IMAGE_ATTRS:
            if k in self.attrs:
                v = self.attrs[k]
                d[k] = v.split(",") if k == "mkimage_options" else v
        d["dtbs"] = list(self.files)
        return d


class Family:
    def __init__(self, name: str, attrs: dict, images: list):
        self.name = name
        self.attrs = attrs
        self.images = images

    @property
    def dtb_prefix(self) -> str:
        return self.attrs.get("dtb_prefix", "")

    def dtbs(self) -> list:
        """Unique DTB names in first-seen order, with the images that carry each."""
        seen: "OrderedDict[str, list]" = OrderedDict()
        for img in self.images:
            for f in img.files:
                seen.setdefault(f, []).append(img.name)
        return list(seen.items())


def parse(text: str) -> "OrderedDict[str, Family]":
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        raise ImportError_("config.xml does not parse: %s" % e)
    if root.tag != "rocknix":
        raise ImportError_("config.xml: root element is <%s>, expected <rocknix>" % root.tag)
    out: "OrderedDict[str, Family]" = OrderedDict()
    for fam in root:
        if not isinstance(fam.tag, str):
            continue  # comments
        fattrs = dict(fam.attrib)
        images = []
        direct = [f for f in fam if f.tag == "file"]
        if direct:
            images.append(Image("", {k: v for k, v in fattrs.items() if k != "dtb_prefix"},
                                [(f.text or "").strip() for f in direct]))
        for sub in fam:
            if not isinstance(sub.tag, str) or sub.tag == "file":
                continue
            attrs = {k: v for k, v in fattrs.items() if k != "dtb_prefix"}
            attrs.update(sub.attrib)
            files = [(f.text or "").strip() for f in sub if f.tag == "file"]
            images.append(Image(sub.tag, attrs, files))
        for img in images:
            for f in img.files:
                if not f or "/" in f or f.endswith(".dtb"):
                    raise ImportError_("config.xml %s/%s: odd DTB name %r" % (fam.tag, img.name, f))
        out[fam.tag] = Family(fam.tag, fattrs, images)
    return out
