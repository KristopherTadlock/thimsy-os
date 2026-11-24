"""Reads an image's AVB hash footer and signs a new image the same way.

The RG Cube's vendor_boot and dtbo carry AVB hash footers signed with AOSP's test key
(testkey_rsa4096.pem). A patched image is re-signed with the footer of the image it was made from:
the same salt, properties, algorithm, flags, and rollback index and location. Re-signing the unpatched image that way gives it back byte for byte (with the
avbtool release that signed it), which patch_dtbo.py checks before it signs anything.

Needs avbtool on PATH; add_footer() also needs AVB_KEY naming the key.
"""
import collections
import os
import subprocess
import sys

FOOTER_FIELDS = {
    "Algorithm:": "algorithm",
    "Rollback Index:": "rollback_index",
    "Rollback Index Location:": "rollback_index_location",
    "Flags:": "flags",
}

# salt: hex string; props: [(key, value)]; opts: {add_hash_footer option: value} (FOOTER_FIELDS);
# partition_name: the hash descriptor's; image_size: the partition size the footer was written for;
# original_size: the size of the image before its footer (the hashed bytes).
Footer = collections.namedtuple("Footer", "salt props opts partition_name image_size original_size")


def size_field(line, label):
    """The byte count in an info_image line such as "Image size:   8388608 bytes"."""
    words = line[len(label):].split()
    if len(words) != 2 or words[1] != "bytes" or not words[0].isdigit():
        return None
    return int(words[0])


def footer_info(image, what="image"):
    """IMAGE's AVB hash footer, as a Footer; WHAT names the image in errors."""
    out = subprocess.run(["avbtool", "info_image", "--image", image], check=True,
                         capture_output=True, text=True).stdout
    salt = partition = image_size = original_size = None
    props, opts = [], {}
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Salt:"):
            salt = line.split()[1]
        elif line.startswith("Prop:"):
            key, _, value = line[len("Prop:"):].strip().partition(" -> ")
            props.append((key, value.strip("'")))
        elif line.startswith("Partition Name:"):
            partition = line[len("Partition Name:"):].strip()
        elif line.startswith("Image size:"):
            image_size = size_field(line, "Image size:")
        elif line.startswith("Original image size:"):
            original_size = size_field(line, "Original image size:")
        else:
            for label, opt in FOOTER_FIELDS.items():
                # The first Flags: is the image's; the hash descriptor's comes later.
                if line.startswith(label) and opt not in opts:
                    opts[opt] = line[len(label):].strip()
    if not salt:
        sys.exit("%s: no hash footer salt" % what)
    missing = sorted(set(FOOTER_FIELDS.values()) - set(opts))
    if not partition:
        missing.append("partition name")
    if image_size is None:
        missing.append("image size")
    if original_size is None:
        missing.append("original image size")
    if missing:
        sys.exit("%s: avbtool info_image shows no %s" % (what, ", ".join(missing)))
    return Footer(salt, props, opts, partition, image_size, original_size)


def add_footer(image, footer, partition_name, partition_size):
    """Signs IMAGE in place for PARTITION_NAME and PARTITION_SIZE with AVB_KEY and FOOTER's salt,
    properties and options (footer_info() of the image it was made from)."""
    key = os.environ.get("AVB_KEY")
    if not key or not os.path.isfile(key):
        sys.exit("AVB_KEY must name the AOSP testkey_rsa4096.pem")
    cmd = ["avbtool", "add_hash_footer", "--image", image, "--partition_name", partition_name,
           "--partition_size", str(partition_size), "--key", key, "--salt", footer.salt]
    for opt, value in sorted(footer.opts.items()):
        cmd += ["--" + opt, value]
    for k, v in footer.props:
        cmd += ["--prop", "%s:%s" % (k, v)]
    subprocess.run(cmd, check=True)
