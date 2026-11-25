/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The BIOS catalogue (see NanoBiosCatalog.h). ~/thimsy/BIOS.md is generated from these tables.

#include "NanoBiosCatalog.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>

namespace android {
namespace nanobios {

namespace {

Slot RA(const char* p, int f = 0) { return Slot{SLOT_RA, p, f}; }
Slot S(int kind, const char* p, int f = 0) { return Slot{kind, p, f}; }
Slot M4D(const char* p) { return Slot{SLOT_MAME4DROID, p, 0}; }

const std::vector<Row> kRows = {
    // Required
    {"ps2", SEC_REQUIRED, true, "PlayStation 2", "ARMSX2", "", "Any other PS2 BIOS that ARMSX2 accepts is installed too."},
    {"vita", SEC_REQUIRED, true, "PS Vita", "Vita3K", "", "Free download from Sony, any version 3.60 or later."},
    {"saturn", SEC_REQUIRED, true, "Saturn", "YabaSanshiro, Beetle Saturn", "", "Beetle Saturn needs the BIOS of the game's region."},
    {"3do", SEC_REQUIRED, true, "3DO", "Opera", "", ""},
    {"lynx", SEC_REQUIRED, false, "Atari Lynx", "Beetle Lynx", "", ""},
    {"coleco", SEC_REQUIRED, false, "ColecoVision", "GearColeco", "", ""},
    // Required for some games
    {"fds", SEC_SOME_GAMES, false, "NES / Famicom", "Mesen, FCEUmm", "Famicom Disk System games", ""},
    {"msx", SEC_SOME_GAMES, false, "MSX", "blueMSX", "MSX disk games", "Cartridge games already run on the built-in free C-BIOS."},
    {"segacd", SEC_SOME_GAMES, true, "Sega CD", "Genesis Plus GX, PicoDrive", "Sega CD games", "Games need the BIOS of their region."},
    {"pcecd", SEC_SOME_GAMES, true, "TurboGrafx-16 / PC Engine", "Beetle PCE Fast", "CD games", ""},
    {"neogeo", SEC_SOME_GAMES, true, "Neo Geo", "FBNeo, MAME, MAME 2003-Plus", "Neo Geo games",
     "Recognised by its contents. MAME 2003-Plus reads it from the game's folder."},
    {"pgm", SEC_SOME_GAMES, true, "PGM", "FBNeo, MAME, MAME 2003-Plus", "PGM games", "Recognised by its contents."},
    {"qsound", SEC_SOME_GAMES, true, "CPS-2", "MAME", "CPS-2 games in MAME", "Recognised by its contents."},
    {"64dd", SEC_SOME_GAMES, false, "Nintendo 64", "M64Plus FZ, Mupen64Plus", "64DD disks", ""},
    {"3dokanji", SEC_SOME_GAMES, false, "3DO", "Opera", "Japanese games that use Kanji fonts", ""},
    // Optional
    {"psx", SEC_OPTIONAL, true, "PlayStation", "Beetle PSX HW, SwanStation, PCSX ReARMed", "", "Much better compatibility; recommended."},
    {"dc", SEC_OPTIONAL, true, "Dreamcast", "Flycast", "", "A few games need it."},
    {"gc", SEC_OPTIONAL, false, "GameCube", "Dolphin", "", "Plays the console's start-up animation."},
    {"nds", SEC_OPTIONAL, false, "Nintendo DS", "SeedlessDS, DraStic", "", ""},
    {"gba", SEC_OPTIONAL, false, "Game Boy Advance", "mGBA, gpSP", "", ""},
    {"gb", SEC_OPTIONAL, false, "Game Boy / Game Boy Color", "SameBoy, mGBA", "", "Shows the console's start-up screen."},
    {"md", SEC_OPTIONAL, false, "Genesis / Master System / Game Gear", "Genesis Plus GX", "", "Shows the console's start-up screen."},
    {"a5200", SEC_OPTIONAL, false, "Atari 5200", "a5200", "", ""},
    {"a7800", SEC_OPTIONAL, false, "Atari 7800", "ProSystem", "", ""},
    // Installed, not listed
    {"ps2extra", SEC_OPTIONAL, false, "PlayStation 2", "ARMSX2", "", "Files dumped together with a PS2 BIOS."},
    {"snes", SEC_SOME_GAMES, false, "Super Nintendo", "bsnes, Snes9x", "Satellaview, Sufami Turbo, Super Game Boy and special-chip games", ""},
    {"stv", SEC_SOME_GAMES, false, "Sega ST-V", "Beetle Saturn, MAME, MAME 2003-Plus", "ST-V games", ""},
    {"psxextra", SEC_OPTIONAL, false, "PlayStation", "SwanStation", "", "The PS1 BIOS of a PS3."},
    {"gbextra", SEC_OPTIONAL, false, "Game Boy / Game Boy Color", "SameBoy", "", "Game Boy games in Game Boy Advance mode."},
    {"gcdsp", SEC_OPTIONAL, false, "GameCube", "Dolphin", "", "DSP ROMs from a GameCube (Dolphin has its own)."},
    {"3donorsa", SEC_OPTIONAL, false, "3DO", "Opera", "", "BIOS variants without the RSA check."},
    {"adam", SEC_SOME_GAMES, false, "ColecoVision", "GearColeco", "ADAM software", ""},
    {"dcflash", SEC_OPTIONAL, false, "Dreamcast", "Flycast", "", "Flash memory (settings) of a Dreamcast."},
    {"ndsfw", SEC_OPTIONAL, false, "Nintendo DS", "DraStic, SeedlessDS", "", "Firmware of your own DS."},
    {"pspfont", SEC_OPTIONAL, false, "PSP", "PPSSPP", "", "Fonts from a PSP's flash0 (PPSSPP has its own)."},
    {"mt32", SEC_OPTIONAL, false, "ScummVM", "ScummVM", "", "Roland MT-32 / CM-32L sound."},
    {"3ds", SEC_OPTIONAL, false, "Nintendo 3DS", "Azahar", "", "From your own 3DS. Not needed for decrypted games: Azahar has its own fonts and system data. The real shared font fixes Chinese, Korean and Taiwanese text."},
    {"xbox", SEC_OPTIONAL, false, "Xbox", "hakuX", "", "Not needed to play: hakuX runs games without them. Only used to bring saves from your own Xbox or from xemu into hakuX."},
    {"atari800", SEC_OPTIONAL, false, "Atari 8-bit", "Atari800", "", "The core has a free OS built in; the real one runs more games."},
    {"amiga", SEC_REQUIRED, false, "Amiga", "PUAE", "", ""},
    {"amigacd", SEC_REQUIRED, false, "Amiga CD32 / CDTV", "PUAE", "", ""},
    {"mac", SEC_REQUIRED, false, "Apple Macintosh", "Mini vMac", "", ""},
    {"scv", SEC_REQUIRED, false, "Super Cassette Vision", "EmuSCV", "", ""},
    {"crvision", SEC_REQUIRED, false, "CreatiVision", "JollyCV", "", ""},
    // MAME4droid 2024 (not shipped): MAME's own sets, the machine's and its devices' zips
    {"m4d_apple2gs", SEC_REQUIRED, false, "Apple IIGS", "MAME4droid 2024", "", ""},
    {"m4d_ti99", SEC_REQUIRED, false, "TI-99/4A", "MAME4droid 2024", "", ""},
    {"m4d_coco", SEC_REQUIRED, false, "Tandy Color Computer", "MAME4droid 2024", "", ""},
    {"m4d_dragon", SEC_REQUIRED, false, "Dragon 32 / 64", "MAME4droid 2024", "", ""},
    {"m4d_fmtowns", SEC_REQUIRED, false, "FM Towns", "MAME4droid 2024", "", ""},
    {"m4d_adam", SEC_REQUIRED, false, "Coleco Adam", "MAME4droid 2024", "", ""},
    {"m4d_arch", SEC_REQUIRED, false, "Acorn Archimedes", "MAME4droid 2024", "", ""},
    {"m4d_electron", SEC_REQUIRED, false, "Acorn Electron", "MAME4droid 2024", "", ""},
    {"m4d_fm7", SEC_REQUIRED, false, "FM-7", "MAME4droid 2024", "", ""},
    {"m4d_oric", SEC_REQUIRED, false, "Oric", "MAME4droid 2024", "", ""},
    {"m4d_samcoupe", SEC_REQUIRED, false, "SAM Coupé", "MAME4droid 2024", "", ""},
    {"m4d_trs80", SEC_REQUIRED, false, "TRS-80", "MAME4droid 2024", "", ""},
    {"m4d_atom", SEC_REQUIRED, false, "Acorn Atom", "MAME4droid 2024", "", ""},
    {"m4d_astrocade", SEC_REQUIRED, false, "Bally Astrocade", "MAME4droid 2024", "", ""},
    {"m4d_gamate", SEC_REQUIRED, false, "Gamate", "MAME4droid 2024", "", ""},
    {"m4d_gamecom", SEC_REQUIRED, false, "Game.com", "MAME4droid 2024", "", ""},
    {"m4d_vsmile", SEC_REQUIRED, false, "V.Smile", "MAME4droid 2024", "", ""},
    {"m4d_supracan", SEC_REQUIRED, false, "Super A'Can", "MAME4droid 2024", "", ""},
    {"m4d_gmaster", SEC_REQUIRED, false, "Game Master", "MAME4droid 2024", "", ""},
    {"m4d_studio2", SEC_REQUIRED, false, "RCA Studio II", "MAME4droid 2024", "", ""},
    {"m4d_advision", SEC_REQUIRED, false, "Adventure Vision", "MAME4droid 2024", "", ""},
    {"m4d_apf", SEC_REQUIRED, false, "APF M-1000", "MAME4droid 2024", "", ""},
};

#define F(row, name, desc, size, sha1, md5, rec, ...) \
    File{row, name, desc, MATCH_SHA1, size, 0, sha1, md5, rec, {__VA_ARGS__}}
#define FN(row, name, desc, size, sizeMax, ...) \
    File{row, name, desc, MATCH_NAME, size, sizeMax, "", "", 0, {__VA_ARGS__}}

const std::vector<File> kFiles = {
    // PlayStation 2 (ARMSX2 checks the image itself, any name)
    F("ps2", "ps2-0230a-20080220.bin", "USA v2.30", 4194304, "f9229fe159d0353b9f0632f3fdc66819c9030458", "21038400dc633070a78ad53090c53017", REC_USA, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0230e-20080220.bin", "Europe v2.30", 4194304, "9915b5ba56798f4027ac1bd8d10abe0c1c9c326a", "dc69f0643a3030aaa4797501b483d6c4", REC_EUR, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0230j-20080220.bin", "Japan v2.30", 4194304, "fbd54bfc020af34008b317dcb80b812dd29b3759", "80ac46fa7e77b8ab4366e86948e54f83", REC_JPN, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0220a-20060210.bin", "USA v2.20", 4194304, "92e488d5b2705e4cca83d4d1efbc421012faf83e", "cb801b7920a7d536ba07b6534d2433ca", 0, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0200a-20040614.bin", "USA v2.00", 4194304, "7a62e5f48603582707e9898eb055ea3eaee50d4c", "d333558cc14561c1fdc334c75d5f37b7", 0, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0200e-20040614.bin", "Europe v2.00", 4194304, "434bc0b4eb4827da0773ec0795aadc5162569a07", "dc752f160044f2ed5fc1f4964db2a095", 0, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0160a-20020207.bin", "USA v1.60", 4194304, "f9a5d629a036b99128f7cb530c6e3ca016e9c8b7", "d5ce2c7d119f563ce04bc04dbc3a323e", 0, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    F("ps2", "ps2-0100j-20000117.bin", "Japan v1.00", 4194304, "aea061e6e263fdcc1c4fdbd68553ef78dae74263", "acf4730ceb38ac9d8c7d8e21f2614600", 0, S(SLOT_ARMSX2, "", SLOTF_SELECT)),
    File{"ps2", "", "Any other PS2 BIOS (checked by ARMSX2)", MATCH_PS2ROMDIR, 4194304, 0, "", "", 0, {S(SLOT_ARMSX2, "", SLOTF_SELECT)}},
    File{"ps2extra", "<BIOS name>.nvm .mec .rom1 .rom2 .erom", "Beside the BIOS, same name", MATCH_COMPANION, 0, 0, "", "", 0,
         {S(SLOT_ARMSX2, "", SLOTF_IF_ABSENT)}},

    // PS Vita: Sony's update packages, recognised by their header (any version)
    File{"vita", "PSVUPDAT.PUP", "System software", MATCH_PUP, 0, 0, "", "", REC_ALL, {S(SLOT_VITA3K, "")}},
    File{"vita", "PSP2UPDAT.PUP", "Font package", MATCH_PUP, 0, 0, "", "", REC_ALL, {S(SLOT_VITA3K, "")}},

    // Saturn: YabaSanshiro reads saturn_bios.bin, then sega_101.bin, then mpr-17933.bin; Beetle
    // Saturn sega_101.bin (Japan) and mpr-17933.bin (USA, Europe).
    F("saturn", "mpr-17933.bin", "USA / Europe", 524288, "faa8ea183a6d7bbe5d4e03bb1332519800d3fbc3", "3240872c70984b6cbfda1586cab68dbe", REC_USA | REC_EUR, RA("")),
    F("saturn", "sega_101.bin", "Japan", 524288, "df94c5b4d47eb3cc404d88b33a8fda237eaf4720", "85ec9ca47d8f6807718151cbcca8b964", REC_JPN, RA("")),
    F("saturn", "saturn_bios.bin", "Japan v1.00", 524288, "2b8cb4f87580683eb4d760e4ed210813d667f0a2", "af5828fdff51384f99b3c4926be27762", 0, RA("", SLOTF_SATURN_SEL)),

    F("lynx", "lynxboot.img", "", 512, "e4ed47fae31693e016b081c6bda48da5b70d7ccb", "fcd403db69f54290b51035d82f835e7b", REC_ALL, RA("")),
    F("coleco", "colecovision.rom", "", 8192, "45bedc4cbdeac66c7df59e9e599195c778d86a92", "2c66f5911e5b42b8ebe113403548eee7", REC_ALL, RA("")),

    // MSX: blueMSX 2.8.2's two folders, copied when both of these are found inside the chosen folder
    // (file by file, never over one: the C-BIOS machines Thimsy ships stay)
    File{"msx", "Databases/msxromdb.xml", "blueMSX 2.8.2", MATCH_FOLDERSET, 930986, 0, "573aa830b7bef4452808b40df883469907db2fed", "", REC_ALL,
         {S(SLOT_RA_FOLDER, "Databases")}},
    File{"msx", "Machines/Shared Roms/MSX.rom", "blueMSX 2.8.2", MATCH_FOLDERSET, 32768, 0, "e998f0c441f4f1800ef44e42cd1659150206cf79", "", REC_ALL,
         {S(SLOT_RA_FOLDER, "Machines")}},

    F("fds", "disksys.rom", "", 8192, "57fe1bdee955bb48d357e463ccbf129496930b62", "ca30b50f880eb660a320674ed365ef7a", REC_ALL, RA("")),

    F("segacd", "bios_CD_U.bin", "USA", 131072, "f4f315adcef9b8feb0364c21ab7f0eaf5457f3ed", "2efd74e3232ff260e371b99f84024f7f", REC_USA, RA("")),
    F("segacd", "bios_CD_E.bin", "Europe", 131072, "f891e0ea651e2232af0c5c4cb46a0cae2ee8f356", "e66fa1dc5820d254611fdcdba0662372", REC_EUR, RA("")),
    F("segacd", "bios_CD_J.bin", "Japan", 131072, "4846f448160059a7da0215a5df12ca160f26dd69", "278a9397d192149e84e820ac621a8edd", REC_JPN, RA("")),

    F("pcecd", "syscard3.pce", "Super CD-ROM2 System 3.0", 262144, "79f5ff55dd10187c7fd7b8daab0b3ffbd1f56a2c", "38179df8f4ac870017db21ebcbf53114", REC_ALL, RA("")),
    F("pcecd", "syscard2.pce", "CD-ROM2 System 2.x", 262144, "88da02e2503f7c32810f5d93a34849d470742b6d", "3cdd6614a918616bfc41c862e889dd79", 0, RA("")),
    F("pcecd", "syscard1.pce", "CD-ROM2 System 1.x", 262144, "a39a66da7de6ba94ab84d04eef7afeec7d4ee66a", "2b7ccb3d86baa18f6402c176f3065082", 0, RA("")),
    F("pcecd", "gexpress.pce", "Game Express CD Card", 32768, "014881a959e045e00f4db8f52955200865d40280", "6d2cb14fc3e1f65ceb135633d1694122", 0, RA("")),

    // Arcade BIOS sets: the whole zip, recognised by its members
    File{"neogeo", "neogeo.zip", "Neo Geo BIOS set", MATCH_ZIPSET, 0, 0, "", "", REC_ALL,
         {RA("fbneo/neogeo.zip"), RA("mame/bios/neogeo.zip")}},
    File{"pgm", "pgm.zip", "PGM BIOS set", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {RA("fbneo/pgm.zip"), RA("mame/bios/pgm.zip")}},
    File{"qsound", "qsound.zip", "QSound sound chip", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {RA("mame/bios/qsound.zip")}},

    F("64dd", "64DD_IPL.bin", "64DD IPL (Japan)", 4194304, "bf861922dcb78c316360e3e742f4f70ff63c9bc3", "8d3d9f294b6e174bc7b1d2fd1c727530", REC_ALL,
      RA("Mupen64plus/IPL.n64"), S(SLOT_MUPEN, "", SLOTF_PREF_64DD)),

    F("3dokanji", "panafz1-kanji.bin", "Panasonic FZ-1 Kanji ROM", 933636, "acd39a8fee1b9d2950d5ab447846c11fb31af63e", "b8dc97f778a6245c58e064b0312e8281", REC_ALL, RA("")),
    F("3dokanji", "panafz1j-kanji.bin", "Panasonic FZ-1J Kanji ROM", 1048576, "884515605ee243577ab20767ef8c1a7368e4e407", "c23fb5d5e6bb1c240d02cf968972be37", REC_ALL, RA("")),
    F("3dokanji", "panafz10ja-anvil-kanji.bin", "Panasonic FZ-10JA Kanji ROM", 1048576, "2e857b957803d0331fd229328df01f3ffab69eee", "428577250f43edc902ea239c50d2240d", REC_ALL, RA("")),

    // PlayStation: each under its own name (every core finds the ones it knows)
    F("psx", "scph5501.bin", "USA", 524288, "0555c6fae8906f3f09baf5988f00e55f88e9f30b", "490f666e1afb15b7362b406ed1cea246", REC_USA, RA("")),
    F("psx", "scph5502.bin", "Europe", 524288, "f6bc2d1f5eb6593de7d089c425ac681d6fffd3f0", "32736f17079d0b2b7024407c39bd3050", REC_EUR, RA("")),
    F("psx", "scph5500.bin", "Japan", 524288, "b05def971d8ec59f346f2d9ac21fb742e3eb6917", "8dd7d5296a650fac7319bce665a6a53c", REC_JPN, RA("")),
    F("psx", "scph101.bin", "USA v4.5", 524288, "dcffe16bd90a723499ad46c641424981338d8378", "6e3735ff4c7dc899ee98981385f6f3d0", 0, RA("")),
    F("psx", "scph7001.bin", "USA v4.1", 524288, "14df4f6c1e367ce097c11deae21566b4fe5647a9", "1e68c231d0896b7eadcad1d7d8e76129", 0, RA("")),
    F("psx", "scph1001.bin", "USA v2.0", 524288, "10155d8d6e6e832d6ea66db9bc098321fb5e8ebf", "924e392ed05558ffdb115408c263dccf", 0, RA("")),
    F("psx", "psxonpsp660.bin", "Region-free, from a PSP", 524288, "96880d1ca92a016ff054be5159bb06fe03cb4e14", "c53ca5908936d412331790f4426c6c33", 0, RA("")),

    F("dc", "dc_boot.bin", "", 2097152, "8951d1bb219ab2ff8583033d2119c899cc81f18c", "e10c53c2f8b90bab96ead2d368858623", REC_ALL, S(SLOT_FLYCAST, ""), RA("dc/dc_boot.bin")),

    // GameCube: one IPL.bin per region folder, the newest version found (best first here)
    F("gc", "IPL.bin", "Europe v1.2", 2097152, "80b8744ff5e43585392f55546bd03a673d11ef5f", "db92574caab77a7ec99d4605fd6f2450", REC_EUR, S(SLOT_DOLPHIN_GC, "EUR/IPL.bin")),
    F("gc", "IPL.bin", "Europe v1.1", 2097152, "f3cd0c7c61cbcefa85e7de3aff4cfa50bc508714", "339848a0b7c2124cf155276c1e79cbd0", 0, S(SLOT_DOLPHIN_GC, "EUR/IPL.bin")),
    F("gc", "IPL.bin", "Europe v1.0", 2097152, "f27c63e5394e2fd1606f70df004c4fc2d6027700", "0cdda509e2da83c85bfe423dd87346cc", 0, S(SLOT_DOLPHIN_GC, "EUR/IPL.bin")),
    F("gc", "IPL.bin", "NTSC v1.2 (USA, Japan)", 2097152, "ef9194ab4804aa0aa8540d846caf291b28331165", "b17148254a5799684c7d783206504926", REC_USA | REC_JPN,
      S(SLOT_DOLPHIN_GC, "USA/IPL.bin"), S(SLOT_DOLPHIN_GC, "JAP/IPL.bin")),
    F("gc", "IPL.bin", "NTSC v1.1 (USA, Japan)", 2097152, "239eacd86527ff9a75aeb7282da65797baeef010", "019e39822a9ca3029124f74dd4d55ac4", 0,
      S(SLOT_DOLPHIN_GC, "USA/IPL.bin"), S(SLOT_DOLPHIN_GC, "JAP/IPL.bin")),
    F("gc", "IPL.bin", "NTSC v1.0 (USA, Japan)", 2097152, "a1837968288253ed541f2b11440b68f5a9b33875", "fc924a7c879b661abc37cec4f018fdf3", 0,
      S(SLOT_DOLPHIN_GC, "USA/IPL.bin"), S(SLOT_DOLPHIN_GC, "JAP/IPL.bin")),

    F("nds", "bios7.bin", "ARM7 BIOS", 16384, "24f67bdea115a2c847c8813a262502ee1607b7df", "df692a80a5b1bc90728bc3dfc76cd948", REC_ALL,
      S(SLOT_DRASTIC, "nds_bios_arm7.bin"), S(SLOT_SEEDLESSDS, "nds_bios_arm7.bin")),
    F("nds", "bios9.bin", "ARM9 BIOS", 4096, "bfaac75f101c135e32e2aaf541de6b1be4c8c62d", "a392174eb3e572fed6447e956bde4b25", REC_ALL,
      S(SLOT_DRASTIC, "nds_bios_arm9.bin"), S(SLOT_SEEDLESSDS, "nds_bios_arm9.bin")),

    F("gba", "gba_bios.bin", "", 16384, "300c20df6731a33952ded8c436f7f186d25d3492", "a860e8c0b6d573d191e4ec7db1b1e4f6", REC_ALL, RA("")),

    F("gb", "dmg_boot.bin", "Game Boy", 256, "4ed31ec6b0b175bb109c0eb5fd3d193da823339f", "32fbbd84168d3482956eb3c5051637f5", REC_ALL, RA(""), RA("gb_bios.bin")),
    F("gb", "cgb_boot.bin", "Game Boy Color", 2304, "1293d68bf9643bc4f36954c1e80e38f39864528d", "dbfce9db9deaa2567f6a84fde55f9680", REC_ALL, RA(""), RA("gbc_bios.bin")),
    F("gb", "sgb_boot.bin", "Super Game Boy", 256, "aa2f50a77dfb4823da96ba99309085a3c6278515", "d574d4f9c12f305074798f54c091a8b4", REC_ALL, RA(""), RA("sgb_bios.bin")),
    F("gb", "sgb2_boot.bin", "Super Game Boy 2", 256, "93407ea10d2f30ab96a314d8eca44fe160aea734", "e0430bca9925fb9882148fd2dc2418c1", REC_ALL, RA("")),

    F("md", "bios_MD.bin", "Genesis", 2048, "3f50b76b0529db7f79c396b5e808cc0786ffc311", "d3293ebaaa7f4eb2a6766b68a0fb4609", REC_ALL, RA("")),
    F("md", "bios_U.sms", "Master System (USA, Europe)", 8192, "c315672807d8ddb8d91443729405c766dd95cae7", "840481177270d5642a14ca71ee72844c", REC_USA | REC_EUR,
      RA(""), RA("bios_E.sms")),
    F("md", "bios_J.sms", "Master System (Japan)", 8192, "a8c1b39a2e41137835eda6a5de6d46dd9fadbaf2", "24a519c53f67b00640d0048ef7089105", REC_JPN, RA("")),
    F("md", "bios.gg", "Game Gear", 1024, "914aa165e3d879f060be77870d345b60cfeb4ede", "672e104c3be3a238301aceffc3b23fd6", REC_ALL, RA("")),

    F("3do", "panafz10.bin", "Panasonic FZ-10", 1048576, "3c912300775d1ad730dc35757e279c274c0acaad", "51f2f43ae2f3508a14d9f56597e2d3ce", REC_ALL, RA("")),
    F("3do", "panafz1.bin", "Panasonic FZ-1", 1048576, "34bf189111295f74d7b7dfc1f304d98b8d36325a", "f47264dd47fe30f73ab3c010015c155b", 0, RA("")),
    F("3do", "panafz1j.bin", "Panasonic FZ-1J (Japan)", 1048576, "ec7ec62d60ec0459a14ed56ebc66761ef3c80efc", "a496cfdded3da562759be3561317b605", 0, RA("")),
    F("3do", "goldstar.bin", "GoldStar GDO-101M", 1048576, "c4a2e5336f77fb5f743de1eea2cda43675ee2de7", "8639fd5e549bd6238cfee79e3e749114", 0, RA("")),
    F("3do", "sanyotry.bin", "Sanyo IMP-21J TRY", 1048576, "b01c53da256dde43ffec4ad3fc3adfa8d635e943", "35fa1a1ebaaeea286dc5cd15487c13ea", 0, RA("")),
    F("3do", "panafz10e-anvil.bin", "Panasonic FZ-10-E (Anvil)", 1048576, "a900371f0cdcdc03f79557f11d406fd71251a5fd", "a48e6746bd7edec0f40cff078f0bb19f", 0, RA("")),

    F("a5200", "5200.rom", "", 2048, "6ad7a1e8c9fad486fbec9498cb48bf5bc3adc530", "281f20ea4320404ec820fb7ec0693b38", REC_ALL, RA("")),
    F("a7800", "7800 BIOS (U).rom", "USA", 4096, "d9d134bb6b36907c615a594cc7688f7bfcef5b43", "0763f1ffb006ddbe32e52d497ee848ae", REC_USA | REC_JPN, RA("")),
    F("a7800", "7800 BIOS (E).rom", "Europe", 16384, "5a140136a16d1d83e4ff32a19409ca376a8df874", "397bb566584be7b9764e7a68974c4263", REC_EUR, RA("")),

    // ---- not listed in the dialog ----
    F("snes", "BS-X.bin", "Satellaview", 1048576, "604556b2e62860af18db5a77f2956ebc75450020", "fed4d8242cfbed61343d53d48432aced", REC_ALL, RA("")),
    F("snes", "STBIOS.bin", "Sufami Turbo", 262144, "ef86ea192eed03d5c413fdbbfd46043be1d7a127", "d3a44ba7d42a74d3ac58cb9c14c6a5ca", REC_ALL, RA("")),
    F("snes", "SGB1.sfc", "Super Game Boy", 262144, "973e10840db683cf3faf61bd443090786b3a9f04", "b15ddb15721c657d82c5bab6db982ee9", REC_ALL, RA("")),
    F("snes", "SGB2.sfc", "Super Game Boy 2", 524288, "e5b2922ca137051059e4269b236d07a22c07bc84", "8ecd73eb4edf7ed7e81aef1be80031d5", REC_ALL, RA("")),
    F("snes", "cx4.data.rom", "Cx4", 3072, "a002f4efba42775a31185d443f3ed1790b0e949a", "037ac4296b6b6a5c47c440188d3c72e3", REC_ALL, RA("")),
    F("snes", "dsp1.data.rom", "DSP-1", 2048, "589c158685fe3cf88c7ca77cb6083f0e4de9c7c3", "3d81b45fa0c2aa8b852dfb1ece7c0971", REC_ALL, RA("")),
    F("snes", "dsp1.program.rom", "DSP-1", 6144, "c5d283604beebb3d832f15a0b3fc60d10dd4bddc", "ae209fbe789fbf11a48aea5ab1197321", REC_ALL, RA("")),
    F("snes", "dsp1b.data.rom", "DSP-1B", 2048, "19d5e822fe15ee6942e9921c3d8275a761a969df", "1e3f568634a7d8284020dddc0ae905bc", REC_ALL, RA("")),
    F("snes", "dsp1b.program.rom", "DSP-1B", 6144, "03c6c0bdb40846c42aeff6b716e1ea0e001c3ae3", "d10f446888e097cbf500f3f663cf4f6d", REC_ALL, RA("")),
    F("snes", "dsp2.data.rom", "DSP-2", 2048, "aed9167cc2897cd8d0432f0b8f713fabbc48f5c4", "e9417e29223b139c3c4b635a2a3b8744", REC_ALL, RA("")),
    F("snes", "dsp2.program.rom", "DSP-2", 6144, "51a274955ef493cfcf2efad7e2ee54738c81eb15", "aa6e5922a3ed5ded54f24247c11143c5", REC_ALL, RA("")),
    F("snes", "dsp3.data.rom", "DSP-3", 2048, "e7ec3da203a4edba2b17c06d9384787304a025a0", "0a81210c0a940b997dd9843281008ee6", REC_ALL, RA("")),
    F("snes", "dsp3.program.rom", "DSP-3", 6144, "11b7ba3501a8542af4cdfdbd1c9702ae0a0e3fae", "d99ca4562818d49cee1f242705bba6f8", REC_ALL, RA("")),
    F("snes", "dsp4.data.rom", "DSP-4", 2048, "8f2ffb9c1702aa2f76191e98f3fef43f73e5aab7", "ee4990879eb68e3cbca239c5bc20303d", REC_ALL, RA("")),
    F("snes", "dsp4.program.rom", "DSP-4", 6144, "1abee528a59238fd5668fdcffd598d079cb056c2", "a151023b948b90ffc23a5b594bb6fef2", REC_ALL, RA("")),
    F("snes", "st010.data.rom", "ST010", 4096, "bd116501ec41c438fff210fd03233c6c9d15a514", "254d70762b6f59f99c27c395aba7d07d", REC_ALL, RA("")),
    F("snes", "st010.program.rom", "ST010", 49152, "2d0f34074ed3c6dfd61f767ee8b18e699b9769ac", "1d70019179a59a566a0bb5d3f2845544", REC_ALL, RA("")),
    F("snes", "st011.data.rom", "ST011", 4096, "798ab84110ca93a6e73a540f4146d21f8d9e1442", "10bd3f4aa949737ab9836512c35bcc29", REC_ALL, RA("")),
    F("snes", "st011.program.rom", "ST011", 49152, "375eca92592621e83881f3a9d7fd5ea3e5b306dc", "95222ebf1c0c2990bcf25db43743f032", REC_ALL, RA("")),
    F("snes", "st018.data.rom", "ST018", 32768, "b19c0f8f207d62fdabf4bf71442826063bccc626", "49c898b60d0f15e90d0ba780dd12f366", REC_ALL, RA("")),
    F("snes", "st018.program.rom", "ST018", 131072, "388e3721b94cd074d6ba0eca8616523d2118a6c3", "dda40ccd57390c96e49d30a041f9a9e7", REC_ALL, RA("")),

    F("stv", "epr-20091.ic8", "Japan", 524288, "eff0f54c70bce05ff3a289bf30b1027e1c8cd117", "", REC_ALL, RA("")),
    F("stv", "epr-17952a.ic8", "USA", 524288, "eaf1c3e5d602e1139d2090a78d7e19f04f916794", "", REC_ALL, RA("")),
    F("stv", "epr-17954a.ic8", "Europe", 524288, "af79cff317e5b57d49e463af16a9f616ed1eee08", "", REC_ALL, RA("")),
    // Beetle Saturn reads the loose files above, else system/stvbios.zip.
    File{"stv", "stvbios.zip", "ST-V BIOS set", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {RA("mame/bios/stvbios.zip"), RA("stvbios.zip")}},

    F("psxextra", "ps1_rom.bin", "From a PS3", 0, "c40146361eb8cf670b19fdc9759190257803cab7", "81bbe60ba7a3d1cea1d48c14cbcc647b", REC_ALL, RA("")),
    FN("gbextra", "agb_boot.bin", "Game Boy Advance", 256, 0, RA("", SLOTF_IF_ABSENT)),
    FN("gcdsp", "dsp_rom.bin", "DSP ROM", 8192, 0, S(SLOT_DOLPHIN_GC, "dsp_rom.bin", SLOTF_IF_ABSENT)),
    FN("gcdsp", "dsp_coef.bin", "DSP coefficients", 4096, 0, S(SLOT_DOLPHIN_GC, "dsp_coef.bin", SLOTF_IF_ABSENT)),

    F("3donorsa", "panafz10-norsa.bin", "Panasonic FZ-10", 1048576, "f05e642322c03694f06a809c0b90fc27ac73c002", "1477bda80dc33731a65468c1f5bcbee9", REC_ALL, RA("")),
    F("3donorsa", "panafz10e-anvil-norsa.bin", "Panasonic FZ-10-E (Anvil)", 1048576, "2765c7b4557cc838b32567d2428d088980295159", "cf11bbb5a16d7af9875cca9de9a15e09", REC_ALL, RA("")),
    F("3donorsa", "panafz1j-norsa.bin", "Panasonic FZ-1J", 1048576, "a417587ae3b0b8ef00c830920c21af8bee88e419", "f6c71de7470d16abe4f71b1444883dc8", REC_ALL, RA("")),

    File{"adam", "eos.rom", "ADAM EOS", MATCH_MD5, 8192, 0, "", "01df3140909f09aa9aac4f88890f676c", REC_ALL, {RA("")}},
    File{"adam", "writer.rom", "ADAM SmartWriter", MATCH_MD5, 32768, 0, "", "4fe4f6800076ea3d897d4285653447bd", REC_ALL, {RA("")}},

    FN("dcflash", "dc_nvmem.bin", "Flash memory", 131072, 0, S(SLOT_FLYCAST, "dc_nvmem.bin", SLOTF_IF_ABSENT), RA("dc/dc_nvmem.bin", SLOTF_IF_ABSENT)),
    FN("dcflash", "dc_flash.bin", "Flash memory", 131072, 0, S(SLOT_FLYCAST, "dc_nvmem.bin", SLOTF_IF_ABSENT), RA("dc/dc_nvmem.bin", SLOTF_IF_ABSENT)),

    FN("ndsfw", "firmware.bin", "DS firmware", 131072, 0, S(SLOT_DRASTIC, "nds_firmware.bin", SLOTF_IF_ABSENT)),
    // SeedlessDS reads only a 256 KB firmware.
    FN("ndsfw", "firmware.bin", "DS Lite firmware", 262144, 0, S(SLOT_DRASTIC, "nds_firmware.bin", SLOTF_IF_ABSENT),
       S(SLOT_SEEDLESSDS, "nds_firmware.bin", SLOTF_IF_ABSENT)),
    FN("ndsfw", "firmware.bin", "DS firmware (iQue)", 524288, 0, S(SLOT_DRASTIC, "nds_firmware.bin", SLOTF_IF_ABSENT)),

    FN("pspfont", "jpn0.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "kr0.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn0.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn1.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn2.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn3.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn4.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn5.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn6.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn7.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn8.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn9.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn10.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn11.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn12.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn13.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn14.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),
    FN("pspfont", "ltn15.pgf", "", 0, 0, S(SLOT_PPSSPP_FONT, "", SLOTF_IF_ABSENT)),

    FN("mt32", "MT32_CONTROL.ROM", "MT-32", 65536, 0, S(SLOT_SCUMMVM, "", SLOTF_IF_ABSENT)),
    FN("mt32", "MT32_PCM.ROM", "MT-32", 524288, 0, S(SLOT_SCUMMVM, "", SLOTF_IF_ABSENT)),
    FN("mt32", "CM32L_CONTROL.ROM", "CM-32L", 65536, 0, S(SLOT_SCUMMVM, "", SLOTF_IF_ABSENT)),
    FN("mt32", "CM32L_PCM.ROM", "CM-32L", 1048576, 0, S(SLOT_SCUMMVM, "", SLOTF_IF_ABSENT)),

    FN("3ds", "boot9.bin", "ARM9 boot ROM", 65536, 0, S(SLOT_AZAHAR, "sysdata/boot9.bin", SLOTF_IF_ABSENT)),
    FN("3ds", "sector0x96.bin", "", 512, 0, S(SLOT_AZAHAR, "sysdata/sector0x96.bin", SLOTF_IF_ABSENT)),
    FN("3ds", "otp.bin", "", 256, 0, S(SLOT_AZAHAR, "sysdata/otp.bin", SLOTF_IF_ABSENT)),
    FN("3ds", "seeddb.bin", "", 0, 0, S(SLOT_AZAHAR, "sysdata/seeddb.bin", SLOTF_IF_ABSENT)),
    FN("3ds", "shared_font.bin", "", 0, 0, S(SLOT_AZAHAR, "sysdata/shared_font.bin", SLOTF_IF_ABSENT)),
    FN("3ds", "keys.txt", "", 0, 0, S(SLOT_AZAHAR, "sysdata/keys.txt", SLOTF_IF_ABSENT)),
    FN("3ds", "movable.sed", "", 288, 320, S(SLOT_AZAHAR, "nand/private/movable.sed", SLOTF_IF_ABSENT)),
    FN("3ds", "SecureInfo_A", "", 273, 0, S(SLOT_AZAHAR, "nand/rw/sys/SecureInfo_A", SLOTF_IF_ABSENT)),
    FN("3ds", "LocalFriendCodeSeed_B", "", 272, 0, S(SLOT_AZAHAR, "nand/rw/sys/LocalFriendCodeSeed_B", SLOTF_IF_ABSENT)),

    // hakuX reads console/mcpx.bin, bios.bin and eeprom.bin. Dumps usually carry xemu's names
    // (mcpx_1.0.bin, Complex_4627.bin); flash.bin is hakuX's older name. The MCPX's MD5 is xemu's;
    // modified BIOSes have no one hash, so those go by name.
    File{"xbox", "mcpx_1.0.bin", "MCPX 1.0 boot ROM", MATCH_MD5, 512, 0, "", "d49c52a4102f6df7bcf8d0617ac475ed", REC_ALL,
         {S(SLOT_HAKUX, "mcpx.bin", SLOTF_IF_ABSENT)}},
    FN("xbox", "Complex_4627.bin", "BIOS (256 KB flash)", 262144, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "Complex_4627.bin", "BIOS (1 MB flash)", 1048576, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "bios.bin", "BIOS (256 KB flash)", 262144, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "bios.bin", "BIOS (1 MB flash)", 1048576, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "flash.bin", "BIOS (256 KB flash)", 262144, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "flash.bin", "BIOS (1 MB flash)", 1048576, 0, S(SLOT_HAKUX, "bios.bin", SLOTF_IF_ABSENT)),
    FN("xbox", "eeprom.bin", "EEPROM", 256, 0, S(SLOT_HAKUX, "eeprom.bin", SLOTF_IF_ABSENT)),

    // Names, sizes and hashes from libretro-database dat/System.dat.
    F("atari800", "ATARIXL.ROM", "XL/XE OS", 16384, "ae4f523ba08b6fd59f3cae515a2b2410bbd98f55", "06daac977823773a3eea3422fd26a703", REC_ALL, RA("")),
    F("atari800", "ATARIBAS.ROM", "BASIC", 8192, "3693c9cb9bf3b41bae1150f7a8264992468fc8c0", "0bac0c6a50104045d902df4503a4c30b", REC_ALL, RA("")),
    F("atari800", "ATARIOSA.ROM", "400/800 OS A", 10240, "6dd53356159a129ed12367beb3b24a771d41adb0", "eb1f32f5d9f382db1bbfb8d7f9cb343a", REC_ALL, RA("")),
    F("atari800", "ATARIOSB.ROM", "400/800 OS B", 10240, "f1f0741b1d34fb4350cf7cb8ab3b6ea11cdd8174", "a3e8d617c95d08031fe1b20d541434b2", REC_ALL, RA("")),
    F("amiga", "kick34005.A500", "Kickstart 1.3 (A500)", 262144, "891e9a547772fe0c6c19b610baf8bc4ea7fcb785", "82a21c1890cae844b3df741f2762d48d", REC_ALL, RA("")),
    F("amiga", "kick40063.A600", "Kickstart 3.1 (A600)", 524288, "3b7f1493b27e212830f989f26ca76c02049f09ca", "e40a5dfb3d017ba8779faba30cbd1c8e", REC_ALL, RA("")),
    F("amiga", "kick40068.A1200", "Kickstart 3.1 (A1200)", 524288, "e21545723fe8374e91342617604f1b3d703094f1", "646773759326fbac3b2311fd8c8793ee", REC_ALL, RA("")),
    F("amigacd", "kick40060.CD32", "CD32 Kickstart", 524288, "3525be8887f79b5929e017b42380a79edfee542d", "5f8924d013dd57a89cf349f4cdedc6b1", REC_ALL, RA("")),
    F("amigacd", "kick40060.CD32.ext", "CD32 extended ROM", 524288, "5bef3d628ce59cc02a66e6e4ae0da48f60e78f7f", "bb72565701b1b6faece07d68ea5da639", REC_ALL, RA("")),
    F("amigacd", "kick34005.CDTV", "CDTV extended ROM", 262144, "7ba40ffa17e500ed9fed041f3424bd81d9c907be", "89da1838a24460e4b93f4f0c5d92d48d", REC_ALL, RA("")),
    F("mac", "MacII.ROM", "Macintosh II", 262144, "db6b504744281369794e26ba71a6e385cf6227fa", "66223be1497460f1e60885eeb35e03cc", REC_ALL, RA("")),
    F("scv", "upd7801g.s01", "", 4096, "6e89d1227581c76441a53d605f9e324185f1da33", "635a978fd40db9a18ee44eff449fc126", REC_ALL, RA("")),
    F("crvision", "bioscv.rom", "", 2048, "5ac620c529e4965efb5560fe824854a44c983757", "3b1ef759d8e3fb4071582efd33dd05f9", REC_ALL, RA("")),

    // MAME4droid 2024: MAME's zips, each under its own name (a device's ROMs in a zip of its name).
    // aa3000's ROMs are aa4401's too; with a split set both use aa310.zip.
    File{"m4d_apple2gs", "apple2gs.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_ti99", "ti99_4a.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_ti99", "ti99_speech.zip", "Speech synthesizer", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_coco", "coco.zip", "Color Computer 1 / 2", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_coco", "coco3.zip", "Color Computer 3", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_coco", "coco_fdc.zip", "Disk controller", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_dragon", "dragon32.zip", "Dragon 32", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_dragon", "dragon64.zip", "Dragon 64", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_dragon", "dragon_fdc.zip", "Disk controller", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_fmtowns", "fmtownsux.zip", "FM Towns II UX", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_fmtowns", "fmtmarty.zip", "FM Towns Marty", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_adam", "adam.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_adam", "adam_kb.zip", "Keyboard", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_adam", "adam_prn.zip", "Printer", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_adam", "adam_ddp.zip", "Digital Data Pack", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_adam", "adam_fdc.zip", "Disk drive", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_arch", "aa310.zip", "Archimedes 310", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_arch", "aa3000.zip", "A3000, A440/1", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D(""), M4D("aa4401.zip")}},
    File{"m4d_electron", "electron64.zip", "Electron 64K", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_electron", "electron_plus3.zip", "Plus 3 disc interface", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_fm7", "fm7.zip", "FM-7", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_fm7", "fm77av.zip", "FM77AV", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_oric", "oric1.zip", "Oric-1", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_oric", "orica.zip", "Oric Atmos", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_samcoupe", "samcoupe.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_trs80", "trs80l2.zip", "Model I Level II", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_atom", "atom.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_atom", "atom_discpack.zip", "Disc Pack", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_astrocade", "astrocde.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_gamate", "gamate.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_gamecom", "gamecom.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_vsmile", "vsmile.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_supracan", "supracan.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_gmaster", "gmaster.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_studio2", "studio2.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_advision", "advision.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
    File{"m4d_apf", "apfm1000.zip", "", MATCH_ZIPSET, 0, 0, "", "", REC_ALL, {M4D("")}},
};

#undef F
#undef FN

// "sp-*": any member whose name starts with "sp-" (a Neo Geo system BIOS).
const std::vector<ZipSet> kZipSets = {
    {"neogeo.zip",
     {{"sp-s3.sp1", 131072, 0x91b64be3, "720a3e20d26818632aedf2c2fd16c54f213543e1"},
      {"sp-s2.sp1", 131072, 0x9036d879, "4f5ed7105b7128794654ce82b51723e16e389543"},
      {"sm1.sm1", 131072, 0x94416d67, "42f9d7ddd6c0931fd64226a60dc73602b2819dcf"},
      {"sfix.sfix", 131072, 0xc2ea0cfd, "fd4a618cdcdbf849374f0a50dd8efe9dbab706c3"},
      {"000-lo.lo", 131072, 0x5a86cff2, "5992277debadeb64d1c1c64b0a92d9293eaf7e4a"},
      // MAME 2003-Plus's names for the same parts (an older set)
      {"sfix.sfx", 131072, 0x354029fc, "4ae4bf23b4c2acff875775d4cbff5583893ce2a1"},
      {"mame.sm1", 131072, 0x97cf998b, "977387a7c76ef9b21d0b01fa69830e949a9a9626"},
      {"mamelo.lo", 65536, 0xe09e253c, "2b1c719531dac9bb503f22644e6e4236b91e7cfc"}},
     {{"sm1.sm1"}, {"sfix.sfix"}, {"000-lo.lo"}, {"sp-s3.sp1", "sp-s2.sp1", "sp-*"}}},
    {"pgm.zip",
     {{"pgm_t01s.rom", 2097152, 0x1a7123a0, "cc567f577bfbf45427b54d6695b11b74f2578af3"},
      {"pgm_m01s.rom", 2097152, 0x45ae7159, "d3ed3ff3464557fd0df6b069b2e431528b0ebfa8"},
      {"pgm_p01s.u20", 131072, 0xe42b166e, "2a9df9ec746b14b74fae48b1a438da14973702ea"},
      {"pgm_p02s.u20", 131072, 0x78c15fa2, "885a6558e022602cc6f482ac9667ba9f61e75092"}},
     {{"pgm_t01s.rom"}, {"pgm_m01s.rom"}, {"pgm_p01s.u20", "pgm_p02s.u20"}}},
    {"qsound.zip", {{"dl-1425.bin", 8192, 0xd6cf5ef5, "555f50fe5cdf127619da7d854c03f4a244a0c501"}}, {{"dl-1425.bin"}}},
    {"stvbios.zip",
     {{"epr-20091.ic8", 524288, 0x59ed40f4, "eff0f54c70bce05ff3a289bf30b1027e1c8cd117"},
      {"epr-17952a.ic8", 524288, 0xd1be2adf, "eaf1c3e5d602e1139d2090a78d7e19f04f916794"},
      {"epr-17954a.ic8", 524288, 0xf7722da3, "af79cff317e5b57d49e463af16a9f616ed1eee08"},
      {"epr-19730.ic8", 524288, 0xd0e0889d, "fae53107c894e0c41c49e191dbe706c9cd6e50bd"}},
     {{"epr-20091.ic8", "epr-17952a.ic8", "epr-17954a.ic8", "epr-19730.ic8"}}},
    // MAME4droid 2024's sets: members from the ROM_START blocks of MAME's source (2026-11), every BIOS
    // version included; needed are the default BIOS's ROMs. A clone needs only the ROMs its parent
    // lacks (a split set), and comes after its parent, so a merged parent zip is never taken for it.
    {"apple2gs.zip",
     {{"341s0632-2.bin", 4096, 0xe1c11fb0, "141d18c36a617ab9dce668445440d34354be0672"},
      {"344s0047.bin", 16384, 0x2d541944, "5a5a77c8ec45632aea1a57cd9c9257f7f6e44668"},
      {"341-0728", 131072, 0x8d410067, "c0f4704233ead14cb8e1e8a68fbd7063c56afd27"},
      {"341-0748", 131072, 0x18190283, "c70576869deec92ca82c78438b1d5c686eac7480"}},
     {{"341s0632-2.bin"}, {"344s0047.bin"}, {"341-0728"}, {"341-0748"}}},
    {"ti99_4a.zip",
     {{"994a_rom_hb.u610", 4096, 0xee859c5f, "a45245707c3dccea902b718554a882d214a82504"},
      {"994a_rom_lb.u611", 4096, 0x37859301, "f4e774fd5913b387a763f1b8de5524c54b255434"},
      {"994a_grom0.u500", 6144, 0x2445a5e8, "ea15d8b0ac52112dc0d5f4ab9a79ac8ca1cc1bbc"},
      {"994a_grom1.u501", 6144, 0xb8f367ab, "3ecead4b83ec525084c70b6123d4053f8a80e1f7"},
      {"994a_grom2.u502", 6144, 0xe0bb5341, "e255f0d65d69b927cecb8fcfac7a4c17d585ea96"}},
     {{"994a_rom_hb.u610"}, {"994a_rom_lb.u611"}, {"994a_grom0.u500"}, {"994a_grom1.u501"}, {"994a_grom2.u502"}}},
    {"ti99_speech.zip",
     {{"cd2325a.u2a", 16384, 0x1f58b571, "0ef4f178716b575a1c0c970c56af8a8d97561ffe"},
      {"cd2326a.u2b", 16384, 0x65d00401, "a367242c2c96cebf0e2bf21862f3f6734b2b3020"}},
     {{"cd2325a.u2a"}, {"cd2326a.u2b"}}},
    {"coco.zip",
     {{"bas10.rom", 8192, 0x00b50aaa, "1f08455cd48ce6a06132aea15c4778f264e19539"},
      {"bas11.rom", 8192, 0x6270955a, "cecb7c24ff1e0ab5836e4a7a8eb1b8e01f1fded3"},
      {"bas12.rom", 8192, 0x54368805, "0f14dc46c647510eb0b7bd3f53e33da07907d04f"},
      {"extbas10.rom", 8192, 0x6111a086, "8aa58f2eb3e8bcfd5470e3e35e2b359e9a72848e"},
      {"extbas11.rom", 8192, 0xa82a6254, "ad927fb4f30746d820cb8b860ebb585e7f095dea"}},
     {{"bas12.rom"}, {"extbas11.rom"}}},
    {"coco3.zip",
     {{"coco3.rom", 32768, 0xb4c88d6c, "e0d82953fb6fd03768604933df1ce8bc51fc427d"}},
     {{"coco3.rom"}}},
    {"coco_fdc.zip",
     {{"disk10.rom", 8192, 0xb4f9968e, "04115be3f97952b9d9310b52f806d04f80b40d03"},
      {"disk11.rom", 8192, 0x0b9c5415, "10bdc5aa2d7d7f205f67b47b19003a4bd89defd1"},
      {"ados.rom", 8192, 0x24e807cf, "a935ea11af4c600a771e4540b661cbb4258a21d6"},
      {"ados2b.rom", 8192, 0x47a59ad4, "66bd3cf08e7f1b318e82e8c4a9848233f38f1a56"},
      {"ados3.rom", 8192, 0x6f824cd1, "de602d8d219094f1237d37a4f80032c25808b358"},
      {"ados3-40.rom", 8192, 0x8afe1a04, "a8dcc6fc0aa5589612cea2a318e7fae58d930c6c"},
      {"ados3-80.rom", 8192, 0x859762f5, "957f7d5a10e61266193b636dbf642002bcedfaa3"},
      {"rgbdos_mess.rom", 8192, 0x0b0e64db, "062ffab14dc788ec7744e528bf9bb425c3ec60ed"},
      {"hdbdw3bck.rom", 8192, 0x867a3f42, "8fd64f1c246489e0bf2b3743ae76332ff324716a"},
      {"hdbdw3bc3.rom", 8192, 0x309a9efd, "671605d61811953860466f771c1594bbade331f4"}},
     {{"disk11.rom"}}},
    {"dragon32.zip",
     {{"dragon_data_ltd_1-0.ic18", 8192, 0xe192b3eb, "9fbba5128b8a53c65ee0586c10513a0a6fb05a7d"},
      {"dragon_data_ltd_1-1.ic17", 8192, 0x0e1d7bf0, "7088d75995cc2ec80a7eed9b9cc3d62f0f820a43"}},
     {{"dragon_data_ltd_1-0.ic18"}, {"dragon_data_ltd_1-1.ic17"}}},
    {"dragon64.zip",
     {{"d64_1.rom", 16384, 0x60a4634c, "f119506eaa3b4b70b9aa0dd83761e8cbe043d042"},
      {"d64_2.rom", 16384, 0x17893a42, "e3c8986bb1d44269c4587b04f1ca27a70b0aaa2e"}},
     {{"d64_1.rom"}, {"d64_2.rom"}}},
    {"dragon_fdc.zip",
     {{"ddos10.rom", 8192, 0xb44536f6, "a8918c71d319237c1e3155bb38620acb114a80bc"}},
     {{"ddos10.rom"}}},
    {"fmtownsux.zip",
     {{"fmt_dos_a.rom", 524288, 0x22270e9f, "a7e97b25ff72b14121146137db8b45d6c66af2ae"},
      {"fmt_dic.rom", 524288, 0x82d1daa2, "7564020dba71deee27184824b84dbbbb7c72aa4e"},
      {"fmt_fnt.rom", 262144, 0xdd6fd544, "a216482ea3162f348fcf77fea78e0b2e4288091a"},
      {"fmt_sys_a.rom", 262144, 0x92f3fa67, "be21404098b23465d24c4201a81c96ac01aff7ab"},
      {"mytownsux.rom", 32, 0x5cc7e6bc, "e245f8086df57ce6e48853f0e13525f738e5c4d8"}},
     {{"fmt_dos_a.rom"}, {"fmt_dic.rom"}, {"fmt_fnt.rom"}, {"fmt_sys_a.rom"}, {"mytownsux.rom"}}},
    {"fmtmarty.zip",
     {{"mrom.m36", 2097152, 0x9c0c060c, "5721c5f9657c570638352fa9acac57fa8d0b94bd"},
      {"mrom.m37", 2097152, 0xfb66bb56, "e273b5fa618373bdf7536495cd53c8aac1cce9a5"}},
     {{"mrom.m36"}, {"mrom.m37"}}},
    {"adam.zip",
     {{"os7.u2", 8192, 0x3aa93ef3, "45bedc4cbdeac66c7df59e9e599195c778d86a92"},
      {"alf @1 rev 57 e3d5.u8", 8192, 0x565b364a, "ebdafad6e268e7ed1674c1fb89607622748a5b36"},
      {"alf @2 rev 57 ae6a.u20", 8192, 0x44a1cff4, "661cdf36d9699d6c21c5f9e205ebc41c707359dd"},
      {"alf @3 rev 57 8534.u21", 8192, 0x77657b90, "d25d32ab6c8fafbc21b4b925b3e644fa26d111f7"},
      {"eos 6 rev 57 08dd.u22", 16384, 0xef6403c5, "28c7616cd02e4286f9b4c1c4a8b8850832b49fcb"},
      {"wp_r80.rom", 32768, 0x58d86a2a, "d4aec4efe1431e56fe52d83baf9118542c525255"},
      {"master rev a 174b.u6", 2048, 0x035a7a3d, "0426e6eaf18c2be9fe08066570c214ab5951ee14"}},
     {{"os7.u2"}, {"alf @1 rev 57 e3d5.u8"}, {"alf @2 rev 57 ae6a.u20"}, {"alf @3 rev 57 8534.u21"}, {"eos 6 rev 57 08dd.u22"}, {"wp_r80.rom"}, {"master rev a 174b.u6"}}},
    {"adam_kb.zip",
     {{"keyboard.u2", 2048, 0xef204746, "83162ffc75847328a05429135b728a63efb05b93"}},
     {{"keyboard.u2"}}},
    {"adam_prn.zip",
     {{"printer.u2", 2048, 0xe8db783b, "32b40679749ad0317c2c9ee9ca619fad6d850ce7"}},
     {{"printer.u2"}}},
    {"adam_ddp.zip",
     {{"tape rev a 8865.u24", 2048, 0x6b9ea1cf, "b970f11e8f443fa130fba02ad1f60da51bf89673"}},
     {{"tape rev a 8865.u24"}}},
    {"adam_fdc.zip",
     {{"adam disk u10 ad 31 rev a 09-27-84.u10", 4096, 0x4b0b7143, "1cb68891c3af80e99efad7e309136ca37244f060"}},
     {{"adam disk u10 ad 31 rev a 09-27-84.u10"}}},
    {"aa310.zip",
     {{"0277,022-02.rom", 131072, 0x03bfe550, "e4f3f1e37b84e716d75a32aa291a9189371daa1c"},
      {"0277,023-02.rom", 131072, 0x89ece77c, "e1979a8d3586c006e3837ff721cfa5439e6394bc"},
      {"0277,024-02.rom", 131072, 0x2302db86, "9fb3761571141bd51936daf03fa4f94fca177356"},
      {"0277,025-02.rom", 131072, 0x1546a0da, "078ef64aa8a7196d0ee4bca9926a74ade0a36173"},
      {"0276,322-01.rom", 131072, 0xe6862d4c, "13d8470f1cb2c1d15530bc7fa8a95ecc4a371cf3"},
      {"0276,323-01.rom", 131072, 0xa9aeb4cf, "f37e744ba0e48861815683b24612b0dd69d6ea8b"},
      {"0276,324-01.rom", 131072, 0x7a175186, "efdee41d9811b05a726af9ba135e0178bb1749e5"},
      {"0276,325-01.rom", 131072, 0x7a8811b8, "7a6b5d7bb94ff5e2a31469ab024c8b2d4d295709"},
      {"0283,022-01.rom", 131072, 0x24291ebf, "758adaf6f73b4041a680cdf9a0b2107da12ca5a0"},
      {"0283,023-01.rom", 131072, 0x44a134f1, "2db7f06e692c3191b2e131d55a1cf997e226c7c6"},
      {"0283,024-01.rom", 131072, 0x997f42b6, "779fcb13ce4107c27a003cdc848e8a5b7e039268"},
      {"0283,025-01.rom", 131072, 0x6335dba2, "0e0631ee43acf3d005f24a4d5c11c5c60e6f29a2"},
      {"0270,601-01.rom", 131072, 0x29e2890b, "2ccdbda7494824180426d66cd38659f6ee55a045"},
      {"0270,602-01.rom", 131072, 0xdd1e4893, "2d39a5027fd164fd9409e38074c68731622406bb"},
      {"0270,603-01.rom", 131072, 0x985a8703, "87376fba36757b311f6c4178c2ac04d8a4ad063a"},
      {"0270,604-01.rom", 131072, 0xf23e4c8d, "27595da90d76d3b25b55e176e0688740c5ce39de"},
      {"0270,251-01.rom", 524288, 0x023115a9, "d3233f76d5750e04ef2bc39d5b2dfd96e6a03c45"},
      {"0270,252-01.rom", 524288, 0x6db01129, "4b801dcce4d268d5e4c2680efa23acb29e4f907f"},
      {"0270,253-01.rom", 524288, 0xd749a9f2, "c53c35b847d300989163f9e779590d1853e8adaf"},
      {"0270,254-01.rom", 524288, 0x5b13c523, "b815bdf31dd99e5b4f2d99790cc28ee8cd907b43"},
      {"0296,041-01.rom", 524288, 0xb7499ef8, "4ab53a53c531bfbecdd441c82d9f4c0517682dde"},
      {"0296,042-01.rom", 524288, 0xd55a854c, "c9308cee92cca2a626d8577ec99485ad58b8da2a"},
      {"0296,043-01.rom", 524288, 0x19bc549a, "88b02bd3df94b56284ffad1c24fa140249b7cb63"},
      {"0296,044-01.rom", 524288, 0xbf86f497, "a200dca6dbee7c0be25a7e5363a6a3e6455a3bf3"},
      {"0296,041-02.rom", 524288, 0x84185879, "2740312b32e9cb8ca6cba9f7b33b68dc0dfab810"},
      {"0296,042-02.rom", 524288, 0xc7584553, "144f8f55f06d6d0752f2f989f4f5c7cec38a43ea"},
      {"0296,043-02.rom", 524288, 0xff5acf17, "f9c9d4eb2f465b44353257594e631d0e3706f651"},
      {"0296,044-02.rom", 524288, 0xe2a3480e, "5b48e8b66ba86568e2225d60f34e201dd5f5d52a"},
      {"0276,146-01.rom", 65536, 0x9c45283c, "9eb5bd7ad0958f194a3416d79d7e01e4c45741e1"},
      {"0276,147-01.rom", 65536, 0xad94e17f, "1c8e39c69d4ae1b674e0f732aaa62a4403998f41"},
      {"0276,148-01.rom", 65536, 0x1ab02f2d, "dd7d216967524e64d1a03076a6081461ec8528c3"},
      {"0276,149-01.rom", 65536, 0x5fd6a406, "790af8a4c74d0f6714d528f7502443ce5898a618"},
      {"cmos_arthur.bin", 256, 0x4fc66ddc, "f0eae9a535505d82ba3488ddb7895434df940d73"},
      {"cmos_riscos2.bin", 256, 0x1ecf3369, "96163285797e0d54017d8d4ae87835328a4658bd"},
      {"cmos_riscos3.bin", 256, 0x96ed59b2, "9dab30b4c3305e1142819687889fca334b532679"}},
     {{"0277,022-02.rom"}, {"0277,023-02.rom"}, {"0277,024-02.rom"}, {"0277,025-02.rom"}, {"cmos_arthur.bin"}}},
    {"aa3000.zip",
     {{"0283,022-01.rom", 131072, 0x24291ebf, "758adaf6f73b4041a680cdf9a0b2107da12ca5a0"},
      {"0283,023-01.rom", 131072, 0x44a134f1, "2db7f06e692c3191b2e131d55a1cf997e226c7c6"},
      {"0283,024-01.rom", 131072, 0x997f42b6, "779fcb13ce4107c27a003cdc848e8a5b7e039268"},
      {"0283,025-01.rom", 131072, 0x6335dba2, "0e0631ee43acf3d005f24a4d5c11c5c60e6f29a2"},
      {"0270,601-01.rom", 131072, 0x29e2890b, "2ccdbda7494824180426d66cd38659f6ee55a045"},
      {"0270,602-01.rom", 131072, 0xdd1e4893, "2d39a5027fd164fd9409e38074c68731622406bb"},
      {"0270,603-01.rom", 131072, 0x985a8703, "87376fba36757b311f6c4178c2ac04d8a4ad063a"},
      {"0270,604-01.rom", 131072, 0xf23e4c8d, "27595da90d76d3b25b55e176e0688740c5ce39de"},
      {"0270,251-01.rom", 524288, 0x023115a9, "d3233f76d5750e04ef2bc39d5b2dfd96e6a03c45"},
      {"0270,252-01.rom", 524288, 0x6db01129, "4b801dcce4d268d5e4c2680efa23acb29e4f907f"},
      {"0270,253-01.rom", 524288, 0xd749a9f2, "c53c35b847d300989163f9e779590d1853e8adaf"},
      {"0270,254-01.rom", 524288, 0x5b13c523, "b815bdf31dd99e5b4f2d99790cc28ee8cd907b43"},
      {"0296,041-01.rom", 524288, 0xb7499ef8, "4ab53a53c531bfbecdd441c82d9f4c0517682dde"},
      {"0296,042-01.rom", 524288, 0xd55a854c, "c9308cee92cca2a626d8577ec99485ad58b8da2a"},
      {"0296,043-01.rom", 524288, 0x19bc549a, "88b02bd3df94b56284ffad1c24fa140249b7cb63"},
      {"0296,044-01.rom", 524288, 0xbf86f497, "a200dca6dbee7c0be25a7e5363a6a3e6455a3bf3"},
      {"0296,041-02.rom", 524288, 0x84185879, "2740312b32e9cb8ca6cba9f7b33b68dc0dfab810"},
      {"0296,042-02.rom", 524288, 0xc7584553, "144f8f55f06d6d0752f2f989f4f5c7cec38a43ea"},
      {"0296,043-02.rom", 524288, 0xff5acf17, "f9c9d4eb2f465b44353257594e631d0e3706f651"},
      {"0296,044-02.rom", 524288, 0xe2a3480e, "5b48e8b66ba86568e2225d60f34e201dd5f5d52a"},
      {"cmos_riscos2.bin", 256, 0x1ecf3369, "96163285797e0d54017d8d4ae87835328a4658bd"},
      {"cmos_riscos3.bin", 256, 0x96ed59b2, "9dab30b4c3305e1142819687889fca334b532679"}},
     {{"0283,022-01.rom"}, {"0283,023-01.rom"}, {"0283,024-01.rom"}, {"0283,025-01.rom"}, {"cmos_riscos2.bin"}}},
    {"electron64.zip",
     {{"basic.rom", 16384, 0x79434781, "4a7393f3a45ea309f744441c16723e2ef447a281"},
      {"os_300.rom", 16384, 0xf80a0cea, "165e42ff4164a842e56f08ebd420d5027af99fdd"}},
     {{"basic.rom"}, {"os_300.rom"}}},
    {"electron_plus3.zip",
     {{"adfs.rom", 16384, 0x3289bdc6, "e7c7a1094d50a3579751df2007269067c8ff6812"},
      {"pres_adfs_113.rom", 16384, 0xf06ca04a, "3c8221d63457c552aa2c9a502db632ce1dea66b4"},
      {"pres_adfs_115.rom", 16384, 0x8f81edc3, "32007425058a7b0f8bd5c17b3c22552aa3a03eca"},
      {"acp_dfs1770_200.rom", 16384, 0x5a3a13c7, "d5dad7ab5a0237c44d0426cd85a8ec86545747e0"},
      {"acp_dfs1770_210.rom", 16384, 0xb0661992, "c62f1290f689788dad6a2df30ace083eb827cffe"}},
     {{"adfs.rom"}}},
    {"fm7.zip",
     {{"fbasic300.rom", 31744, 0x87c98494, "d7e3603b0a2442c7632dad45f9704d9ad71968f5"},
      {"subsys_c.rom", 10240, 0x24cec93f, "50b7283db6fe1342c6063fc94046283f4feddc1c"},
      {"tl11-11.m152", 2048, 0x3585ea2a, "51580d4221b93c3d3a5c05bc5dfed8f395e43c2d"},
      {"kanji.rom", 131072, 0x62402ac9, "bf52d22b119d54410dad4949b0687bb0edf3e143"}},
     {{"fbasic300.rom"}, {"subsys_c.rom"}, {"tl11-11.m152"}, {"kanji.rom"}}},
    {"fm77av.zip",
     {{"initiate.rom", 8192, 0x785cb06c, "b65987e98a9564a82c85eadb86f0204eee5a5c93"},
      {"fbasic30.rom", 31744, 0xa96d19b6, "8d5f0cfe7e0d39bf2ab7e4c798a13004769c28b2"},
      {"subsys_c.rom", 10240, 0x24cec93f, "50b7283db6fe1342c6063fc94046283f4feddc1c"},
      {"subsys_a.rom", 8192, 0xe8014fbb, "038cb0b42aee9e933b20fccd6f19942e2f476c83"},
      {"subsys_b.rom", 8192, 0x9be69fac, "0305bdd44e7d9b7b6a17675aff0a3330a08d21a8"},
      {"subsyscg.rom", 8192, 0xe9f16c42, "8ab466b1546d023ba54987790a79e9815d2b7bb2"},
      {"kanji.rom", 131072, 0x62402ac9, "bf52d22b119d54410dad4949b0687bb0edf3e143"}},
     {{"initiate.rom"}, {"fbasic30.rom"}, {"subsys_a.rom"}, {"subsys_b.rom"}, {"subsyscg.rom"}}},
    {"oric1.zip",
     {{"basic10.rom", 16384, 0xf18710b4, "333116e6884d85aaa4dfc7578a91cceeea66d016"},
      {"basic10uk.rom", 16384, 0xd6006a01, "84759cc8faae53070f1c8ce8408982a0edcb0796"}},
     {{"basic10.rom"}}},
    {"orica.zip",
     {{"basic11b.rom", 16384, 0xc3a92bef, "9451a1a09d8f75944dbd6f91193fc360f1de80ac"},
      {"basic12.rom", 16384, 0xdc4f22dc, "845e1a893de3dc0f856fdf2f69c3b73770b4094f"},
      {"basic121.rom", 16384, 0x0a2860b1, "b727d5c3bbc8cb1d510f224eb1e0d90d609e8506"},
      {"basic122.rom", 16384, 0x5ef2a861, "9ab6dc47b6e9dc65a4137ce0f0f12fc2b6ca8442"},
      {"bas11_de.rom", 16384, 0x65233b2d, "b01cabb1a21980a6785a2fe37a8f8572c892123f"},
      {"bas11_es.rom", 16384, 0x47bf26c7, "4fdbadd68db9ab8ad1cd56b4e5cbe51a9c3f11ae"},
      {"bas11_fr.rom", 16384, 0x603b1fbf, "2a4583df3b59ca454d67d5631f242c96ec4cf99a"},
      {"bas11_se.rom", 16384, 0xa71523ac, "ce53acf84baec6ab5cbac9f9cefa71b3efeb2ead"},
      {"bas11_uk.rom", 16384, 0x303370d1, "589ff66fac8e06d65af3369491faa67a71f1322a"},
      {"bas12es_le.rom", 16384, 0x70de4aeb, "b327418aa7d8a5a03c135e3d8acdd511df625893"},
      {"bas12fr_le.rom", 16384, 0x47a437fc, "70271bc3ed5c3bf4d339d6f5de3de8c3c50ff573"},
      {"bas12ge_le.rom", 16384, 0xf5f0dd52, "75359302452ee7b19537698f124aaefd333688d0"},
      {"bas12sw_le.rom", 16384, 0x100abe68, "6211d5969c4d7a6acb86ed19c5e51a33a3bef431"},
      {"bas12uk_le.rom", 16384, 0x00fce8a6, "d40558bdf61b8aba6260293c9424fd463be7fad8"},
      {"bas121es_le.rom", 16384, 0x87ec679b, "5de6a5f5121f69069c9b93d678046e814b5b64e9"},
      {"bas121fr_le.rom", 16384, 0xe683dec2, "20df7ebc0f13aa835f286d50137f1a7ff7430c29"},
      {"bas121ge_le.rom", 16384, 0x94fe32bf, "1024776d20030d602e432e50014502524658643a"},
      {"bas121sw_le.rom", 16384, 0xe6ad11c7, "309c94a9861fcb770636dcde1801a5c68ca819b4"},
      {"bas121uk_le.rom", 16384, 0x75aa1aa9, "ca99e244d9cbef625344c2054023504a4f9dcfe4"},
      {"bas122es_le.rom", 16384, 0x9144f9e0, "acf2094078af057e74a31d90d7010be51b9033fa"},
      {"bas122fr_le.rom", 16384, 0x370cfda4, "fad9a0661256e59bcc2915578647573e4128e1bb"},
      {"bas122ge_le.rom", 16384, 0x9a42bd62, "8a9c80f314daf4e5e64fa202e583b8a65796db8b"},
      {"bas122sw_le.rom", 16384, 0xe7fd57a4, "c75cbf7cfafaa02712dc7ca2f972220aef86fb8d"},
      {"bas122uk_le.rom", 16384, 0x9865bcd7, "2a92e2d119463e682bf10647e3880e26656d65b5"}},
     {{"basic11b.rom"}}},
    {"samcoupe.zip",
     {{"rom31.z5", 32768, 0x0b7e3585, "c86601633fb61a8c517f7657aad9af4e6870f2ee"},
      {"rom30.z5", 32768, 0xe535c25d, "d390f0be420dfb12b1e54a4f528b5055d7d97e2a"},
      {"rom25.z5", 32768, 0xddadd358, "a25ed85a0f1134ac3a481a3225f24a8bd3a790cf"},
      {"rom24.z5", 32768, 0xbb23fee4, "10cd911ba237dd2cf0c2637be1ad6745b7cc89b9"},
      {"rom21.z5", 32768, 0xf6804b46, "11dcac5fdea782cdac03b4d0d7ac25d88547eefe"},
      {"rom20.z5", 32768, 0xeaf32054, "41736323f0236649f2d5fe111f900def8db93a13"},
      {"rom181.z5", 32768, 0xd25e1de1, "cb0fa79e4d5f7df0b57ede08ea7ecc9ae152f534"},
      {"rom18.z5", 32768, 0xf626063f, "485e7d9e9a4f8a70c0f93cd6e69ff12269438829"},
      {"rom14.z5", 32768, 0x08799596, "b4e596051f2748dee9481ea4af7d15ccddc1e1b5"},
      {"rom13.z5", 32768, 0x2093768c, "af8d348fd080b18a4cbe9ed69d254be7be330146"},
      {"rom12.z5", 32768, 0x7fe37dd8, "9339a0c1f72e8512c6f32dec15ab7d6c3bb04151"},
      {"rom10.z5", 32768, 0x3659d31f, "d3de7bb74e04d5b4dc7477f70de54d540b1bcc07"},
      {"rom04.z5", 32768, 0xf439e84e, "8bc457a5c764b0bb0aa7008c57f28c30248fc6a4"},
      {"rom01.z5", 32768, 0xc04acfdf, "8976ed005c14905eec1215f0a5c28aa686a7dda2"},
      {"atom.z5", 32768, 0xdec75f58, "cd342579f066c0863e4f769c2e6757085e21b0a1"}},
     {{"rom31.z5"}}},
    {"trs80l2.zip",
     {{"rom-a.z1", 4096, 0x37c59db2, "e8f8f6a4460a6f6755873580be6ff70cebe14969"},
      {"rom-b.z2", 4096, 0x05818718, "43c538ca77623af6417474ca5b95fb94205500c1"},
      {"rom-c.z3", 4096, 0x306e5d66, "1e1abcfb5b02d4567cf6a81ffc35318723442369"},
      {"rom-a_alt.z1", 4096, 0xbe46faf5, "0e63fc11e207bfd5288118be5d263e7428cc128b"},
      {"rom-b_alt.z2", 4096, 0x6c791c2d, "2a38e0a248f6619d38f1a108eea7b95761cf2aee"},
      {"rom-c_alt.z3", 4096, 0x55b3ad13, "6279f6a68f927ea8628458b278616736f0b3c339"},
      {"mcm6670p.z29", 1024, 0x0033f2b9, "0d2cd4197d54e2e872b515bbfdaa98efe502eda7"}},
     {{"rom-a.z1"}, {"rom-b.z2"}, {"rom-c.z3"}, {"mcm6670p.z29"}}},
    {"atom.zip",
     {{"abasic.ic20", 8192, 0x289b7791, "0072c83458a9690a3ea1f6094f0f38cf8e96a445"},
      {"afloat.ic21", 4096, 0x81d86af7, "ebcde5b36cb3a3344567cbba4c7b9fde015f4802"}},
     {{"abasic.ic20"}, {"afloat.ic21"}}},
    {"atom_discpack.zip",
     {{"dosrom.ic15", 4096, 0xc431a9b7, "71ea0a4b8d9c3caf9718fc7cc279f4306a23b39c"}},
     {{"dosrom.ic15"}}},
    {"astrocde.zip",
     {{"astro.bin", 8192, 0xebc77f3a, "b902c941997c9d150a560435bf517c6a28137ecc"}},
     {{"astro.bin"}}},
    {"gamate.zip",
     {{"gamate_bios_umc.bin", 4096, 0x07090415, "ea449dc607601f9a68d855ad6ab53800d2e99297"},
      {"gamate_bios_bit.bin", 4096, 0x03a5f3a7, "4e9dfbfe916ca485530ef4221593ab68738e2217"}},
     {{"gamate_bios_umc.bin"}}},
    {"gamecom.zip",
     {{"internal.bin", 4096, 0xa0cec361, "03368237e8fed4a8724f3b4a1596cf4b17c96d33"},
      {"external.bin", 262144, 0xe235a589, "97f782e72d738f4d7b861363266bf46b438d9b50"}},
     {{"internal.bin"}, {"external.bin"}}},
    {"vsmile.zip",
     {{"vsmile_v103.bin", 2097152, 0x387fbc24, "5f2fd211b6ff3a6f5121b14adc6bbf4f49e89f33"},
      {"vsmile_v102.bin", 2097152, 0x0cd0bdf5, "5c8d1eada1b6b545555b8d2b09325d7127681af8"},
      {"vsmile_v100.bin", 2097152, 0x205c5296, "7fbcf761b5885c8b1524607aabaf364b4559c8cc"}},
     {{"vsmile_v103.bin"}}},
    {"supracan.zip",
     {{"internal_68k.bin", 4096, 0x8d575662, "a8e75633662978d0a885f16a4ed0f898f278a10a"},
      {"internal_6502_1.bin", 8192, 0xfc9fb05f, "8bf17bf311afeb9974bee058ba63eef5e8b6f5c1"},
      {"internal_6502_2.bin", 8192, 0xbf950ab7, "ab8f15506308b89d2f8ef01b88aa2595d4e1e779"}},
     {{"internal_68k.bin"}, {"internal_6502_1.bin"}, {"internal_6502_2.bin"}}},
    {"gmaster.zip",
     {{"d78c11agf_e19.u1", 4096, 0x05cc45e5, "05d73638dea9657ccc2791c0202d9074a4782c1e"}},
     {{"d78c11agf_e19.u1"}}},
    {"studio2.zip",
     {{"84932.ic11", 512, 0x283b7e65, "4b6d21cde59712ecb5941ff63d8eb161420b0aac"},
      {"84933.ic12", 512, 0xa396b77c, "023517f67af61790e6916b6c4dbe2d9dc07ae3ff"},
      {"85456.ic13", 512, 0xd25cf97f, "d489f41f1125c76cc8ed9defa82a877ae014ef21"},
      {"85457.ic14", 512, 0x74aa724f, "085832f29e0d2a387c75463d66c54fb6c1e9e72c"}},
     {{"84932.ic11"}, {"84933.ic12"}, {"85456.ic13"}, {"85457.ic14"}}},
    {"advision.zip",
     {{"ins8048-11kdp_n.u5", 1024, 0x279e33d1, "bf7b0663e9125c9bfb950232eab627d9dbda8460"},
      {"cop411l-kcn_n.u8", 512, 0x81e95975, "8b6f8c30dd3e9d8e43f1ea20fba2361b383790eb"}},
     {{"ins8048-11kdp_n.u5"}, {"cop411l-kcn_n.u8"}}},
    {"apfm1000.zip",
     {{"apf_4000.rom", 2048, 0xcc6ac840, "1110a234bcad99bd0894ad44c591389d16376ca4"},
      {"trash-ii.bin", 4096, 0x3bd8640a, "da4cd8163990adbc5acd3eab604b41e1066bb832"},
      {"mod_bios.bin", 4096, 0xf320aba6, "9442349fca8b001a5765e2fe8b84db4ece7886c1"}},
     {{"apf_4000.rom"}}},
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string hex(const unsigned char* d, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += kHex[d[i] >> 4]; s += kHex[d[i] & 15]; }
    return s;
}

bool preadAll(int fd, void* buf, size_t len, off_t off) {
    size_t got = 0;
    while (got < len) {
        const ssize_t n = pread(fd, (char*)buf + got, len - got, off + (off_t)got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

}  // namespace

const std::vector<Row>& rows() { return kRows; }
namespace {
const std::vector<File>* gTestFiles = nullptr;
}  // namespace
const std::vector<File>& files() { return gTestFiles ? *gTestFiles : kFiles; }
void setCatalogForTest(const std::vector<File>* f) { gTestFiles = f; }
const std::vector<ZipSet>& zipSets() { return kZipSets; }

const Row* findRow(const char* id) {
    for (const auto& r : kRows) if (!strcmp(r.id, id)) return &r;
    return nullptr;
}

const ZipSet* findZipSet(const std::string& name) {
    for (const auto& z : kZipSets) if (name == z.name) return &z;
    return nullptr;
}

std::string sizeText(const File& f) {
    auto one = [](uint32_t n) {
        if (n >= (1u << 20) && n % (1u << 20) == 0) return std::to_string(n >> 20) + " MB";
        if (n >= 1024 && n % 1024 == 0) return std::to_string(n >> 10) + " KB";
        return std::to_string(n) + " bytes";
    };
    if (f.match == MATCH_ZIPSET) return "zip";
    if (f.match == MATCH_FOLDERSET) return "folder";
    if (f.match == MATCH_PUP || f.match == MATCH_COMPANION || !f.size) return "any size";
    if (f.sizeMax) return one(f.size) + " to " + one(f.sizeMax);
    return one(f.size);
}

const char* allFilesIntro() {
    return "Thimsy recognises these files by their contents, whatever they are called, and copies each one where its "
           "emulators look for it, under the name they need. A file already there is never replaced. Never installed: "
           "games and files that belong to a game, cheat cartridges, and Flycast's arcade BIOS sets. MSX cartridge games "
           "run on C-BIOS, which is built in.";
}

std::vector<AllGroup> allGroups() {
    std::vector<AllGroup> out;
    for (const auto& r : kRows) {
        AllGroup* g = nullptr;
        for (auto& x : out) if (x.system == r.system) g = &x;
        if (!g) { out.push_back(AllGroup{r.system, "", {}, {}}); g = &out.back(); }
        g->rows.push_back(&r);
        // The emulators of all its rows, each once.
        const std::string em = r.emulators;
        for (size_t p = 0; p <= em.size();) {
            size_t q = em.find(", ", p);
            if (q == std::string::npos) q = em.size();
            const std::string one = em.substr(p, q - p);
            bool have = false;
            for (size_t a = 0; a <= g->emulators.size() && !one.empty();) {
                size_t b = g->emulators.find(", ", a);
                if (b == std::string::npos) b = g->emulators.size();
                if (g->emulators.compare(a, b - a, one) == 0 && b - a == one.size()) { have = true; break; }
                a = b + 2;
            }
            if (!have && !one.empty()) g->emulators += (g->emulators.empty() ? "" : ", ") + one;
            p = q + 2;
        }
        bool setDone = false;
        for (const auto& f : kFiles) {
            if (strcmp(f.row, r.id)) continue;
            AllFile a{&f, f.name, f.desc, sizeText(f), "", "Put it anywhere in the folder you pick (any name)"};
            switch (f.match) {
                case MATCH_SHA1: a.check = f.sha1; if (!f.size) a.how = "Must keep this exact name"; break;   // found by name, then hash
                case MATCH_MD5: a.check = std::string("MD5 ") + f.md5; break;
                case MATCH_NAME: a.check = f.size ? "matched by name and size" : "matched by name"; a.how = "Must keep this exact name"; break;
                case MATCH_ZIPSET: a.check = "recognised by its contents"; a.how = "Put it anywhere in the folder you pick (any name; zips are opened)"; break;
                case MATCH_PS2ROMDIR: a.name = "Any other PS2 BIOS"; a.desc = ""; a.check = "checked by ARMSX2"; break;
                case MATCH_COMPANION: a.check = "matched by name"; a.how = "Next to its PS2 BIOS, same base name"; break;
                case MATCH_PUP:
                    a.check = "recognised by its contents";
                    a.how = strcmp(f.name, "PSVUPDAT.PUP") == 0 ? "The PSVUPDAT.PUP file; Vita3K installs it, which takes about a minute"
                                                                : "The PSP2UPDAT.PUP file; Vita3K installs it, which takes about a minute";
                    break;
                case MATCH_FOLDERSET:   // one entry for the two folders
                    if (setDone) continue;
                    setDone = true;
                    a.name = "Machines, Databases"; a.desc = "blueMSX 2.8.2";
                    a.check = "folder";
                    a.how = "Copy the Machines and Databases folders as they are";
                    break;
            }
            g->files.push_back(a);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const AllGroup& a, const AllGroup& b) {
        return strcasecmp(a.system.c_str(), b.system.c_str()) < 0;
    });
    return out;
}

const char* needNothingLine() {
    return "PSP, Xbox, 3DS (decrypted games), N64 cartridges, Virtual Boy, Atari 2600, WonderSwan, Neo Geo Pocket, "
           "PICO-8, 32X, and CPS-1/CPS-2 in FBNeo need no BIOS.";
}

Region regionForLocale(const std::string& langIn, const std::string& regionIn) {
    const std::string lang = lower(langIn);
    std::string region = regionIn;
    for (auto& c : region) c = (char)toupper((unsigned char)c);
    static const char* const kAmericas[] = {"US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "VE", "EC", "BO", "PY",
                                            "UY", "CR", "PA", "GT", "HN", "SV", "NI", "DO", "PR", "CU", "419"};
    // Europe and the other PAL countries
    static const char* const kPal[] = {"GB", "IE", "DE", "AT", "CH", "LI", "FR", "BE", "LU", "MC", "IT", "SM", "ES", "PT",
                                       "AD", "NL", "SE", "DK", "FI", "NO", "IS", "PL", "CZ", "SK", "HU", "RO", "BG", "GR",
                                       "CY", "MT", "SI", "HR", "RS", "BA", "ME", "MK", "AL", "EE", "LV", "LT", "UA", "BY",
                                       "MD", "RU", "TR", "IL", "AU", "NZ", "ZA"};
    if (region == "JP") return REGION_JPN;
    for (const char* r : kAmericas) if (region == r) return REGION_USA;
    for (const char* r : kPal) if (region == r) return REGION_EUR;
    if (lang == "ja") return REGION_JPN;
    static const char* const kEuropean[] = {"de", "fr", "it", "es", "pt", "nl", "sv", "da", "fi", "nb", "no", "nn", "pl",
                                            "el", "ru", "tr", "he", "iw", "cs", "sk", "hu", "ro", "bg", "uk", "hr",
                                            "sr", "sl", "et", "lv", "lt", "is", "ga", "ca", "eu", "gl"};
    for (const char* l : kEuropean) if (lang == l) return REGION_EUR;
    return REGION_USA;   // English without a region, and the languages of other regions
}

const char* slotPackage(int kind) {
    switch (kind) {
        case SLOT_RA: case SLOT_RA_FOLDER: return "com.retroarch.aarch64";
        case SLOT_ARMSX2: return "com.armsx2";
        case SLOT_DOLPHIN_GC: return "org.dolphinemu.dolphinemu";
        case SLOT_FLYCAST: return "com.flycast.emulator";
        case SLOT_DRASTIC: return "com.dsemu.drastic";
        case SLOT_MUPEN: return "org.mupen64plusae.v3.fzurita";
        case SLOT_PPSSPP_FONT: return "org.ppsspp.ppsspp";
        case SLOT_SCUMMVM: return "org.scummvm.scummvm";
        case SLOT_AZAHAR: return "org.azahar_emu.azahar";
        case SLOT_HAKUX: return "com.rfandango.haku_x";
        case SLOT_VITA3K: return "org.vita3k.emulator";
        case SLOT_SEEDLESSDS: return "com.seedlessds.app";
        case SLOT_MAME4DROID: return "com.seleuco.mame4d2024";
        default: return "";
    }
}

const char* slotAppName(int kind) {
    switch (kind) {
        case SLOT_RA: case SLOT_RA_FOLDER: return "RetroArch";
        case SLOT_ARMSX2: return "ARMSX2";
        case SLOT_DOLPHIN_GC: return "Dolphin";
        case SLOT_FLYCAST: return "Flycast";
        case SLOT_DRASTIC: return "DraStic";
        case SLOT_DRASTIC_NANO: return "built-in DS player";
        case SLOT_MUPEN: return "M64Plus FZ";
        case SLOT_PPSSPP_FONT: return "PPSSPP";
        case SLOT_SCUMMVM: return "ScummVM";
        case SLOT_AZAHAR: return "Azahar";
        case SLOT_HAKUX: return "hakuX";
        case SLOT_VITA3K: return "Vita3K";
        case SLOT_SEEDLESSDS: return "SeedlessDS";
        case SLOT_MAME4DROID: return "MAME4droid 2024";
        default: return "";
    }
}

const char* slotWhere(int kind) {
    switch (kind) {
        case SLOT_RA: case SLOT_RA_FOLDER:
            return "RetroArch's system folder (system_directory in retroarch.cfg; by default Internal storage/RetroArch/system)";
        case SLOT_ARMSX2: return "Internal storage/Android/data/com.armsx2/files/bios";
        case SLOT_DOLPHIN_GC: return "Internal storage/Android/data/org.dolphinemu.dolphinemu/files/GC";
        case SLOT_FLYCAST: return "Flycast's home folder/data (by default Internal storage/Android/data/com.flycast.emulator/files/data)";
        case SLOT_DRASTIC: return "DraStic's system folder (by default its app folder, files/DraStic/system)";
        case SLOT_DRASTIC_NANO: return "/data/system/drastic-nano/DraStic/system (Thimsy's built-in DS player)";
        case SLOT_MUPEN: return "Internal storage/Android/data/org.mupen64plusae.v3.fzurita/files (and its 64DD IPL setting)";
        case SLOT_PPSSPP_FONT: return "PPSSPP's memory stick/PSP/flash0/font";
        case SLOT_SCUMMVM: return "ScummVM's extra path (if unset: its app folder, files/extra, set as the extra path)";
        case SLOT_AZAHAR: return "Azahar's folder (only when one has been chosen in Azahar)";
        case SLOT_HAKUX: return "hakuX's app folder, files/console";
        case SLOT_VITA3K: return "installed by Vita3K itself into its memory card folder (pref-path)";
        case SLOT_SEEDLESSDS: return "SeedlessDS's app folder, files/SeedlessDS/system";
        case SLOT_MAME4DROID: return "Internal storage/Android/data/com.seleuco.mame4d2024/files/roms";
        default: return "";
    }
}

const char* slotWhereMoved(int kind) {
    switch (kind) {
        case SLOT_RA: case SLOT_RA_FOLDER: return "microSD card/emulator_config/retroarch/system";
        case SLOT_ARMSX2: return "unchanged (the BIOS stays on internal storage)";
        case SLOT_DOLPHIN_GC: return "microSD card/emulator_config/dolphin/GC (the GC folder becomes a link to it)";
        case SLOT_FLYCAST: return "microSD card/emulator_config/flycast/data";
        case SLOT_DRASTIC: return "microSD card/emulator_config/drastic/system";
        case SLOT_DRASTIC_NANO: return "unchanged";
        case SLOT_MUPEN: return "unchanged";
        case SLOT_PPSSPP_FONT: return "microSD card/emulator_config/ppsspp/PSP/flash0/font";
        case SLOT_SCUMMVM: return "unchanged";
        case SLOT_AZAHAR: return "microSD card/emulator_config/azahar";
        case SLOT_HAKUX: return "unchanged";
        case SLOT_VITA3K: return "microSD card/emulator_config/vita3k (moves with Vita3K's memory card)";
        case SLOT_SEEDLESSDS: return "unchanged";
        case SLOT_MAME4DROID: return "unchanged";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// Matchers
// ---------------------------------------------------------------------------

bool nameMatches(const File& f, const std::string& name, uint64_t size) {
    if (f.match != MATCH_NAME || strcasecmp(name.c_str(), f.name) != 0) return false;
    if (!f.size) return true;
    return f.sizeMax ? size >= f.size && size <= f.sizeMax : size == f.size;
}

int openRegular(const std::string& path, bool noFollow) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (noFollow ? O_NOFOLLOW : 0));
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    const int fl = fcntl(fd, F_GETFL);
    if (fl != -1) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

std::string sha1Hex(const void* data, size_t len) {
    unsigned char d[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)data, len, d);
    return hex(d, sizeof(d));
}

bool hashFile(const std::string& path, std::string* sha1, std::string* md5, bool noFollow) {
    const int fd = openRegular(path, noFollow);
    if (fd < 0) return false;
    SHA_CTX s;
    MD5_CTX m;
    SHA1_Init(&s);
    MD5_Init(&m);
    std::vector<unsigned char> buf(1 << 16);
    bool ok = true;
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        SHA1_Update(&s, buf.data(), (size_t)n);
        if (md5) MD5_Update(&m, buf.data(), (size_t)n);
    }
    close(fd);
    if (!ok) return false;
    unsigned char d[SHA_DIGEST_LENGTH], dm[MD5_DIGEST_LENGTH];
    SHA1_Final(d, &s);
    MD5_Final(dm, &m);
    if (sha1) *sha1 = hex(d, sizeof(d));
    if (md5) *md5 = hex(dm, sizeof(dm));
    return true;
}

bool zipDirectory(const std::string& path, std::vector<ZipEntry>* out) {
    out->clear();
    const int fd = openRegular(path, true);   // the scan resolved links
    if (fd < 0) return false;
    struct stat st;
    bool ok = false;
    if (fstat(fd, &st) == 0 && st.st_size >= 22) {
        // The end of central directory record: within the last 64 KiB + 22 bytes (its comment).
        const off_t tail = std::min<off_t>(st.st_size, 65536 + 22);
        std::vector<unsigned char> t((size_t)tail);
        if (preadAll(fd, t.data(), t.size(), st.st_size - tail)) {
            for (off_t i = tail - 22; i >= 0; i--) {
                const unsigned char* e = &t[(size_t)i];
                if (le32(e) != 0x06054b50) continue;
                const uint16_t count = le16(e + 10);
                const uint32_t cdSize = le32(e + 12), cdOff = le32(e + 16);
                if (count == 0xffff || cdOff == 0xffffffff || count > 4096 || cdSize > (1u << 20)
                    || (off_t)cdOff + cdSize > st.st_size) break;
                std::vector<unsigned char> cd(cdSize);
                if (!preadAll(fd, cd.data(), cd.size(), cdOff)) break;
                size_t p = 0;
                ok = true;
                for (int k = 0; k < count; k++) {
                    if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) { ok = false; break; }
                    ZipEntry z;
                    z.method = le16(&cd[p + 10]);
                    z.crc = le32(&cd[p + 16]);
                    z.csize = le32(&cd[p + 20]);
                    z.usize = le32(&cd[p + 24]);
                    const uint16_t nl = le16(&cd[p + 28]), xl = le16(&cd[p + 30]), cl = le16(&cd[p + 32]);
                    z.localOffset = le32(&cd[p + 42]);
                    if (p + 46 + nl > cd.size()) { ok = false; break; }
                    z.name.assign((const char*)&cd[p + 46], nl);
                    p += 46 + (size_t)nl + xl + cl;
                    out->push_back(std::move(z));
                }
                break;
            }
        }
    }
    close(fd);
    return ok;
}

std::string zipMemberSha1(const std::string& path, const ZipEntry& e) {
    if (e.usize > 64u * 1024 * 1024 || e.csize > 64u * 1024 * 1024 || (e.method != 0 && e.method != 8)) return std::string();
    const int fd = openRegular(path, true);   // the scan resolved links
    if (fd < 0) return std::string();
    std::string result;
    unsigned char lh[30];
    struct stat st;
    if (fstat(fd, &st) == 0 && preadAll(fd, lh, sizeof(lh), e.localOffset) && le32(lh) == 0x04034b50
        && (off_t)e.localOffset + 30 + le16(lh + 26) + le16(lh + 28) + (off_t)e.csize <= st.st_size) {
        const off_t data = (off_t)e.localOffset + 30 + le16(lh + 26) + le16(lh + 28);
        std::vector<unsigned char> comp(e.csize);
        if (e.csize == 0 || preadAll(fd, comp.data(), comp.size(), data)) {
            if (e.method == 0) {
                if (e.csize == e.usize) result = sha1Hex(comp.data(), comp.size());
            } else {
                std::vector<unsigned char> raw(e.usize ? e.usize : 1);
                z_stream zs = {};
                if (inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
                    zs.next_in = comp.data();
                    zs.avail_in = (uInt)comp.size();
                    zs.next_out = raw.data();
                    zs.avail_out = (uInt)e.usize;
                    const int rc = inflate(&zs, Z_FINISH);
                    if ((rc == Z_STREAM_END || (rc == Z_BUF_ERROR && zs.avail_out == 0)) && zs.total_out == e.usize)
                        result = sha1Hex(raw.data(), e.usize);
                    inflateEnd(&zs);
                }
            }
        }
    }
    close(fd);
    return result;
}

const ZipSet* zipMatchSet(const std::string& path, std::vector<std::string>* found) {
    std::vector<ZipEntry> dir;
    if (!zipDirectory(path, &dir)) return nullptr;
    for (const auto& set : kZipSets) {
        std::vector<std::string> ok;   // known members whose directory entry and contents match
        for (const auto& m : set.members) {
            for (const auto& e : dir) {
                std::string base = e.name.substr(e.name.find_last_of('/') + 1);
                if (lower(base) != m.name || e.usize != m.size || e.crc != m.crc) continue;
                if (*m.sha1 && zipMemberSha1(path, e) != m.sha1) continue;
                ok.push_back(m.name);
                break;
            }
        }
        if (ok.empty()) continue;
        bool usable = true;
        for (const auto& group : set.need) {
            bool any = false;
            for (const char* n : group) {
                const size_t len = strlen(n);
                if (len && n[len - 1] == '*') {   // a prefix: any member so named
                    for (const auto& e : dir) {
                        const std::string base = lower(e.name.substr(e.name.find_last_of('/') + 1));
                        if (base.compare(0, len - 1, n, len - 1) == 0) { any = true; break; }
                    }
                } else if (std::find(ok.begin(), ok.end(), std::string(n)) != ok.end()) {
                    any = true;
                }
                if (any) break;
            }
            if (!any) { usable = false; break; }
        }
        if (!usable) continue;
        if (found) *found = ok;
        return &set;
    }
    return nullptr;
}

std::string ps2Romver(const std::string& path) {
    const int fd = openRegular(path, true);   // the scan resolved links
    if (fd < 0) return std::string();
    std::vector<unsigned char> rom(4194304);
    const bool ok = preadAll(fd, rom.data(), rom.size(), 0);
    close(fd);
    if (!ok) return std::string();
    // The ROMDIR: 16-byte entries (name[10], extinfo size u16, file size u32) starting with RESET;
    // each file follows the previous one, 16-byte aligned, from the start of the image.
    size_t dir = std::string::npos;
    for (size_t p = 0; p + 16 <= rom.size(); p += 16)
        if (!memcmp(&rom[p], "RESET\0", 6)) { dir = p; break; }
    if (dir == std::string::npos) return std::string();
    uint64_t off = 0;
    for (size_t p = dir; p + 16 <= rom.size(); p += 16) {
        const unsigned char* e = &rom[p];
        if (!e[0] || strnlen((const char*)e, 10) == 10) break;
        const uint32_t size = le32(e + 12);
        if (!memcmp(e, "ROMVER\0", 7)) {
            if (off + 14 > rom.size()) return std::string();
            std::string v((const char*)&rom[off], 14);
            for (int i = 0; i < 4; i++) if (!isdigit((unsigned char)v[i])) return std::string();
            if (!isalpha((unsigned char)v[4])) return std::string();
            return v;
        }
        off += (size + 15) & ~15ull;
    }
    return std::string();
}

std::string ps2RomverLabel(const std::string& romver, int* region) {
    if (region) *region = -1;
    if (romver.size() < 6) return std::string();
    std::string zone;
    switch (romver[4]) {
        case 'J': zone = "Japan"; if (region) *region = REGION_JPN; break;
        case 'A': zone = "USA"; if (region) *region = REGION_USA; break;
        case 'E': zone = "Europe"; if (region) *region = REGION_EUR; break;
        case 'H': zone = "Asia"; break;
        case 'C': zone = "China"; break;
        default: zone = std::string(1, romver[4]); break;
    }
    const int maj = (romver[0] - '0') * 10 + (romver[1] - '0');
    return zone + " v" + std::to_string(maj) + "." + romver.substr(2, 2);
}

bool isVitaPup(const std::string& path, uint32_t* fwVersion) {
    const int fd = openRegular(path, true);   // the scan resolved links
    if (fd < 0) return false;
    unsigned char h[0x20];
    const bool ok = preadAll(fd, h, sizeof(h), 0) && !memcmp(h, "SCEUF", 5);
    close(fd);
    if (ok && fwVersion) *fwVersion = le32(h + 0x10);
    return ok;
}

}  // namespace nanobios
}  // namespace android
