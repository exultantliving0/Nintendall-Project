#!/usr/bin/env python3
"""Makes the game information files shipped with Nintendall (the gameinfo folder).

They come from the LaunchBox Games Database (https://gamesdb.launchbox-app.com) and TheGamesDB
(https://thegamesdb.net), whose public databases are downloaded once a month: one file per
console, "<console key>.txt", with a line per game:

    <name key> TAB <title> TAB <release date> TAB <players> TAB <back cover picture>

The name key is the game's name in lower case with only its letters and numbers, without "the"
(src/gameinfo.cpp makes the same key from the game's file name to find it). Lines whose second
part starts with "=" are other names of a game: "<name key> TAB =<the game's name key>".
The back cover picture is LaunchBox's when it has one ("lb:<file name>", on images.launchbox-app.com),
else TheGamesDB's (a path on cdn.thegamesdb.net/images/large/).

"manuals.txt" lists the manuals on Digitpress (https://www.digitpress.com/library/manuals/), whose
scans are small PDFs that load fast: "<console key> TAB <name key> TAB <web address>".

If the databases can't be downloaded (no internet) and no earlier copy is saved, no files are made:
the program then shows games without this information.
"""
import argparse
import html
import io
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from xml.etree import ElementTree

DUMP_URL = "https://cdn.thegamesdb.net/json/database-latest.json"
LAUNCHBOX_URL = "https://gamesdb.launchbox-app.com/Metadata.zip"
# LaunchBox's platforms -> Nintendall's console keys (Famicom Disk System games are with the NES ones).
LAUNCHBOX_PLATFORMS = {
    "Nintendo Entertainment System": "nes",
    "Nintendo Famicom Disk System": "nes",
    "Super Nintendo Entertainment System": "snes",
    "Nintendo 64": "n64",
    "Nintendo Game Boy": "gb",
    "Nintendo Game Boy Color": "gbc",
    "Nintendo Game Boy Advance": "gba",
    "Nintendo DS": "nds",
    "Nintendo 3DS": "3ds",
    "Nintendo GameCube": "gamecube",
    "Nintendo Wii": "wii",
    "Nintendo Wii U": "wiiu",
}
# LaunchBox's regions: the USA release's picture is preferred, like the box covers.
LAUNCHBOX_REGION_RANK = {"North America": 5, "United States": 5, "": 4, "World": 4, "Canada": 3,
                         "Europe": 2, "United Kingdom": 2, "Australia": 2}
DIGITPRESS = "https://www.digitpress.com/library/manuals/"
# Digitpress's folders -> Nintendall's console keys.
DIGITPRESS_FOLDERS = {"nes": ["nes"], "snes": ["snes"], "gameboy": ["gb", "gbc"]}
MAX_AGE = 30 * 24 * 3600  # download the database again once a month

# TheGamesDB platform IDs -> Nintendall's console keys (src/systems.cpp).
PLATFORMS = {
    7: "nes",
    6: "snes",
    3: "n64",
    4: "gb",
    41: "gbc",
    5: "gba",
    8: "nds",
    2: "gamecube",
    9: "wii",
    38: "wiiu",
    4912: "3ds",
}

# TheGamesDB regions: 1 NTSC, 2 NTSC-U (USA). The USA release is preferred, like the box covers.
REGION_RANK = {1: 4, 2: 4, 0: 2, 9: 2, 6: 3, 7: 3, 8: 3, 3: 1, 4: 1, 5: 1}

# Accented letters -> plain ones (the same table as src/gameinfo.cpp).
FOLD = {}
for plain, accented in {
    "a": "àáâãäåā", "c": "çć", "e": "èéêëē", "i": "ìíîïī", "n": "ñ", "o": "òóôõöøō",
    "u": "ùúûüū", "y": "ýÿ",
}.items():
    for ch in accented:
        FOLD[ch] = plain
        FOLD[ch.upper()] = plain


def name_key(name):
    s = "".join(FOLD.get(ch, ch) for ch in name).lower().replace("&", " and ")
    words = [w for w in re.findall(r"[a-z0-9]+", s) if w != "the"]
    return "".join(words)


def clean(text):
    text = html.unescape(text or "").replace("\r\n", "\n").replace("\r", "\n").strip()
    return text.replace("\t", " ")


def download(url, cache, check, what):
    """The file at url, saved in cache (downloaded again once a month). None if it isn't available."""
    fresh = os.path.exists(cache) and time.time() - os.path.getmtime(cache) < MAX_AGE
    if not fresh:
        print(f"Downloading {what}...", flush=True)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Nintendall-build"})
            with urllib.request.urlopen(req, timeout=300) as r:
                data = r.read()
            check(data)  # complete and valid
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache + ".part", "wb") as f:
                f.write(data)
            os.replace(cache + ".part", cache)
        except Exception as e:  # offline: use the saved copy, if any
            print(f"Couldn't download {what} ({e})", file=sys.stderr)
    if not os.path.exists(cache):
        return None
    with open(cache, "rb") as f:
        return f.read()


def load_dump(cache):
    data = download(DUMP_URL, cache, json.loads, "TheGamesDB's database")
    return None if data is None else json.loads(data)


def load_launchbox(cache):
    """LaunchBox's games by console and name key: {console: {key: [title, date, players, back]}},
    and their other names {console: {other key: key}}. None if it isn't available."""
    data = download(LAUNCHBOX_URL, cache, lambda d: zipfile.ZipFile(io.BytesIO(d)).getinfo("Metadata.xml"),
                    "the LaunchBox Games Database")
    if data is None:
        return None
    games, aliases = {}, {}
    ids = {}  # database ID -> (console, key)
    backs = {}  # database ID -> (rank, file name)
    with zipfile.ZipFile(io.BytesIO(data)) as z, z.open("Metadata.xml") as f:
        for _, el in ElementTree.iterparse(f):
            tag = el.tag
            if tag == "Game":
                console = LAUNCHBOX_PLATFORMS.get(el.findtext("Platform") or "")
                title = clean(el.findtext("Name"))
                key = name_key(title)
                if console and key:
                    gid = el.findtext("DatabaseID")
                    fds = el.findtext("Platform") == "Nintendo Famicom Disk System"
                    have = games.setdefault(console, {})
                    if key not in have or (not fds and have[key][4]):
                        have[key] = [title, (el.findtext("ReleaseDate") or el.findtext("ReleaseYear") or "")[:10],
                                     el.findtext("MaxPlayers") or "", "", fds]
                    ids[gid] = (console, key)
            elif tag == "GameAlternateName":
                where = ids.get(el.findtext("DatabaseID"))
                k = name_key(clean(el.findtext("AlternateName")))
                if where and k and k != where[1]:
                    aliases.setdefault(where[0], {}).setdefault(k, where[1])
            elif tag == "GameImage":
                gid = el.findtext("DatabaseID")
                if el.findtext("Type") == "Box - Back" and gid in ids and el.findtext("FileName"):
                    rank = LAUNCHBOX_REGION_RANK.get(el.findtext("Region") or "", 1)
                    if gid not in backs or rank > backs[gid][0]:
                        backs[gid] = (rank, el.findtext("FileName"))
            else:
                continue
            el.clear()
    # The back of the box: the game's own (from the release with the best region of any of its entries).
    best = {}
    for gid, (rank, name) in backs.items():
        where = ids[gid]
        if where not in best or rank > best[where][0]:
            best[where] = (rank, name)
    for (console, key), (rank, name) in best.items():
        games[console][key][3] = "lb:" + name
    return games, aliases


def has_links(data):
    if b".pdf" not in data.lower():
        raise ValueError("no manuals listed")


def write_manuals(dest, cache):
    """manuals.txt: the manuals on Digitpress, by console and name key."""
    lines = []
    for folder, consoles in DIGITPRESS_FOLDERS.items():
        page = download(DIGITPRESS + folder + "/index.html", os.path.join(cache, "digitpress-" + folder + ".html"),
                        has_links, f"Digitpress's list of {folder} manuals")
        if page is None:
            continue
        found = {}
        for href in re.findall(r'href="([^"]+\.pdf)"', page.decode("latin-1"), re.I):
            name = html.unescape(href.rsplit("/", 1)[-1][:-4])
            # "Metroid v1", "6 in 1 (Caltron)", "Tetris (UK)": the plain name first, then the others.
            plain = re.sub(r"\s*\([^)]*\)", "", name)
            plain = re.sub(r"\s+v\d+$", "", plain, flags=re.I)
            key = name_key(plain)
            rank = (plain != name, "(uk)" in name.lower(), name)
            if key and (key not in found or rank < found[key][0]):
                found[key] = (rank, DIGITPRESS + folder + "/" + urllib.parse.quote(href))
        for key, (rank, url) in sorted(found.items()):
            for console in consoles:
                lines.append(f"{console}\t{key}\t{url}")
    if lines:
        with open(os.path.join(dest, "manuals.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(sorted(lines)) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", required=True, help="folder to write the files to")
    ap.add_argument("--cache", required=True, help="where to keep the downloaded database")
    args = ap.parse_args()

    os.makedirs(args.dest, exist_ok=True)
    write_manuals(args.dest, args.cache)
    launchbox = load_launchbox(os.path.join(args.cache, "launchbox-metadata.zip"))
    lb_games, lb_aliases = launchbox if launchbox else ({}, {})
    dump = load_dump(os.path.join(args.cache, "database-latest.json"))
    if dump is None and launchbox is None:
        print("No game information (the game databases aren't available)", file=sys.stderr)
        return 0
    pictures = dump.get("include", {}).get("boxart", {}).get("data", {}) if dump else {}

    # Every release of a game (USA, Europe, Japan...) by console and name key.
    groups = {}
    aliases = {}
    for g in dump["data"]["games"] if dump else []:
        console = PLATFORMS.get(g.get("platform"))
        title = clean(g.get("game_title"))
        if not console or not title:
            continue
        key = name_key(title)
        if not key:
            continue
        groups.setdefault(console, {}).setdefault(key, []).append(g)
        for other in g.get("alternates") or []:
            k = name_key(clean(other))
            if k and k != key:
                aliases.setdefault(console, {}).setdefault(k, key)

    def back(g):
        for p in pictures.get(str(g["id"]), []):
            if p.get("type") == "boxart" and p.get("side") == "back" and p.get("filename"):
                return p["filename"]
        return ""

    for console in sorted(set(groups) | set(lb_games)):
        games = groups.get(console, {})
        lb = lb_games.get(console, {})
        entries = {}
        for key, releases in games.items():
            releases.sort(key=lambda g: (-REGION_RANK.get(g.get("region_id"), 0), g["id"]))
            best = releases[0]
            picture = next((back(g) for g in releases if back(g)), "")
            players = best.get("players")
            if not players:
                players = next((g.get("players") for g in releases if g.get("players")), "")
            entries[key] = [clean(best["game_title"]), best.get("release_date") or "", str(players or ""), picture]
        for key, (title, date, players, picture, fds) in lb.items():
            mine = entries.setdefault(key, [title, date, players, ""])
            if not mine[1]:
                mine[1] = date
            if not mine[2]:
                mine[2] = players
            if picture:  # LaunchBox's back of the box first
                mine[3] = picture
        lines = []
        for key, (title, date, players, picture) in sorted(entries.items()):
            if picture or date or players:
                lines.append("\t".join([key, title, date, players, picture]))
        others = dict(lb_aliases.get(console, {}))
        others.update(aliases.get(console, {}))
        for k, key in sorted(others.items()):
            if k not in entries and key in entries:
                lines.append(f"{k}\t={key}")
        with open(os.path.join(args.dest, console + ".txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    print(f"Game information written to {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
