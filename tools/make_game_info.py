#!/usr/bin/env python3
"""Makes the game information files shipped with Nintendall (the gameinfo folder).

They come from TheGamesDB's public database (https://thegamesdb.net, downloaded once a month): one
file per console, "<console key>.txt", with a line per game:

    <name key> TAB <title> TAB <release date> TAB <players> TAB <back cover picture> TAB <description>

The name key is the game's name in lower case with only its letters and numbers, without "the"
(src/gameinfo.cpp makes the same key from the game's file name to find it). Lines whose second
part starts with "=" are other names of a game: "<name key> TAB =<the game's name key>".
In the description, a new line is written as \\n and a backslash as \\\\.

If the database can't be downloaded (no internet) and no earlier copy is saved, no files are made:
the program then shows games without this information.
"""
import argparse
import html
import json
import os
import re
import sys
import time
import urllib.request

DUMP_URL = "https://cdn.thegamesdb.net/json/database-latest.json"
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


def escape(text):
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def load_dump(cache):
    fresh = os.path.exists(cache) and time.time() - os.path.getmtime(cache) < MAX_AGE
    if not fresh:
        print("Downloading TheGamesDB's database...", flush=True)
        try:
            req = urllib.request.Request(DUMP_URL, headers={"User-Agent": "Nintendall-build"})
            with urllib.request.urlopen(req, timeout=300) as r:
                data = r.read()
            json.loads(data)  # complete and valid
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache + ".part", "wb") as f:
                f.write(data)
            os.replace(cache + ".part", cache)
        except Exception as e:  # offline: use the saved copy, if any
            print(f"Couldn't download TheGamesDB's database ({e})", file=sys.stderr)
    if not os.path.exists(cache):
        return None
    with open(cache, "rb") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", required=True, help="folder to write the files to")
    ap.add_argument("--cache", required=True, help="where to keep the downloaded database")
    args = ap.parse_args()

    dump = load_dump(os.path.join(args.cache, "database-latest.json"))
    if dump is None:
        print("No game information (TheGamesDB's database isn't available)", file=sys.stderr)
        return 0
    pictures = dump.get("include", {}).get("boxart", {}).get("data", {})

    # Every release of a game (USA, Europe, Japan...) by console and name key.
    groups = {}
    aliases = {}
    for g in dump["data"]["games"]:
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

    os.makedirs(args.dest, exist_ok=True)
    for console, games in groups.items():
        lines = []
        for key, releases in sorted(games.items()):
            releases.sort(key=lambda g: (-REGION_RANK.get(g.get("region_id"), 0), g["id"]))
            best = releases[0]

            def back(g):
                for p in pictures.get(str(g["id"]), []):
                    if p.get("type") == "boxart" and p.get("side") == "back" and p.get("filename"):
                        return p["filename"]
                return ""

            overview = next((clean(g.get("overview")) for g in releases if clean(g.get("overview"))), "")
            picture = next((back(g) for g in releases if back(g)), "")
            players = best.get("players")
            if not players:
                players = next((g.get("players") for g in releases if g.get("players")), "")
            date = best.get("release_date") or ""
            if not overview and not picture:
                continue
            lines.append("\t".join([key, clean(best["game_title"]), date, str(players or ""), picture,
                                    escape(overview)]))
        for k, key in sorted(aliases.get(console, {}).items()):
            if k not in games and key in games:
                lines.append(f"{k}\t={key}")
        with open(os.path.join(args.dest, console + ".txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    print(f"Game information written to {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
