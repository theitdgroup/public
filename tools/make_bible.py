"""Build data/bible/verses.txt for the watch's Bible screen.

    python tools/make_bible.py

Fetches the King James Version text (public domain) of each reference below from
bible-api.com, so the wording is exact rather than typed from memory. Results are
cached in tools/.bible-cache.json, so re-running only fetches new references.
Output: one verse per line, "Reference<TAB>Text". Verses too long for the round
screen are skipped.
"""
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "tools", ".bible-cache.json")
OUT = os.path.join(ROOT, "data", "bible", "verses.txt")
MAX_CHARS = 250  # about what fits on the dial in a readable font

REFS = """
Genesis 1:1; Genesis 1:27; Genesis 1:31; Genesis 2:7; Genesis 9:13; Genesis 12:2; Genesis 18:14; Genesis 28:15; Genesis 50:20
Exodus 14:14; Exodus 15:2; Exodus 20:12; Exodus 33:14
Leviticus 19:18
Numbers 6:24-26; Numbers 23:19
Deuteronomy 6:5; Deuteronomy 7:9; Deuteronomy 31:6; Deuteronomy 31:8
Joshua 1:8; Joshua 1:9; Joshua 24:15
Ruth 1:16
1 Samuel 2:2; 1 Samuel 12:24; 1 Samuel 16:7; 1 Samuel 17:47
2 Samuel 22:2-3; 2 Samuel 22:31
1 Kings 8:56
1 Chronicles 16:11; 1 Chronicles 16:34; 1 Chronicles 29:11
2 Chronicles 7:14; 2 Chronicles 15:7; 2 Chronicles 16:9; 2 Chronicles 20:15
Nehemiah 8:10; Nehemiah 9:6
Job 19:25; Job 23:10; Job 33:4
Psalms 1:1-2; Psalms 4:8; Psalms 5:3; Psalms 9:1; Psalms 9:10; Psalms 16:8; Psalms 16:11; Psalms 18:2; Psalms 19:1; Psalms 19:14
Psalms 23:1; Psalms 23:4; Psalms 23:6; Psalms 24:1; Psalms 25:4-5; Psalms 27:1; Psalms 27:14; Psalms 28:7; Psalms 29:11; Psalms 30:5
Psalms 31:24; Psalms 32:8; Psalms 33:4; Psalms 34:1; Psalms 34:4; Psalms 34:8; Psalms 34:18; Psalms 37:4; Psalms 37:5; Psalms 37:7
Psalms 37:23-24; Psalms 40:1; Psalms 42:11; Psalms 46:1; Psalms 46:10; Psalms 51:10; Psalms 55:22; Psalms 56:3; Psalms 62:1; Psalms 62:8
Psalms 63:1; Psalms 66:20; Psalms 68:19; Psalms 71:14; Psalms 73:26; Psalms 84:11; Psalms 86:5; Psalms 90:12; Psalms 91:1; Psalms 91:2
Psalms 91:11; Psalms 94:19; Psalms 95:1; Psalms 96:1; Psalms 100:4; Psalms 100:5; Psalms 103:1; Psalms 103:2; Psalms 103:12; Psalms 107:1
Psalms 112:7; Psalms 118:6; Psalms 118:24; Psalms 119:11; Psalms 119:105; Psalms 119:114; Psalms 121:1-2; Psalms 121:7-8; Psalms 126:3; Psalms 127:1
Psalms 130:5; Psalms 136:1; Psalms 138:8; Psalms 139:14; Psalms 143:8; Psalms 145:3; Psalms 145:8; Psalms 145:18; Psalms 147:3; Psalms 150:6
Proverbs 1:7; Proverbs 3:3; Proverbs 3:5-6; Proverbs 3:9; Proverbs 4:23; Proverbs 10:12; Proverbs 11:25; Proverbs 12:25; Proverbs 14:30; Proverbs 15:1
Proverbs 16:3; Proverbs 16:9; Proverbs 16:24; Proverbs 17:17; Proverbs 17:22; Proverbs 18:10; Proverbs 18:24; Proverbs 19:21; Proverbs 22:6; Proverbs 27:17
Proverbs 31:25; Proverbs 31:30
Ecclesiastes 3:1; Ecclesiastes 3:11; Ecclesiastes 4:9; Ecclesiastes 9:10; Ecclesiastes 12:13
Song of Solomon 8:7
Isaiah 6:8; Isaiah 9:6; Isaiah 12:2; Isaiah 25:1; Isaiah 26:3; Isaiah 26:4; Isaiah 30:15; Isaiah 33:2; Isaiah 40:8; Isaiah 40:29
Isaiah 40:31; Isaiah 41:10; Isaiah 41:13; Isaiah 43:1; Isaiah 43:2; Isaiah 43:19; Isaiah 49:15; Isaiah 54:10; Isaiah 55:8; Isaiah 55:11
Isaiah 58:11; Isaiah 60:1; Isaiah 61:1; Isaiah 64:8
Jeremiah 17:7; Jeremiah 29:11; Jeremiah 29:13; Jeremiah 31:3; Jeremiah 32:17; Jeremiah 33:3
Lamentations 3:22-23; Lamentations 3:25
Ezekiel 36:26
Daniel 2:20; Daniel 12:3
Hosea 6:3
Joel 2:13; Joel 2:28
Micah 6:8; Micah 7:7
Nahum 1:7
Habakkuk 3:18; Habakkuk 3:19
Zephaniah 3:17
Zechariah 4:6
Malachi 3:10
Matthew 4:4; Matthew 5:3; Matthew 5:4; Matthew 5:5; Matthew 5:6; Matthew 5:7; Matthew 5:8; Matthew 5:9; Matthew 5:14; Matthew 5:16
Matthew 5:44; Matthew 6:21; Matthew 6:24; Matthew 6:33; Matthew 6:34; Matthew 7:7; Matthew 7:12; Matthew 11:28; Matthew 11:29; Matthew 11:30
Matthew 16:26; Matthew 17:20; Matthew 18:20; Matthew 19:26; Matthew 22:37; Matthew 22:39; Matthew 24:35; Matthew 25:40; Matthew 28:19; Matthew 28:20
Mark 9:23; Mark 10:27; Mark 10:45; Mark 11:24; Mark 12:30; Mark 16:15
Luke 1:37; Luke 2:11; Luke 6:31; Luke 6:38; Luke 11:9; Luke 12:34; Luke 18:27; Luke 19:10
John 1:1; John 1:5; John 1:12; John 1:14; John 3:16; John 3:17; John 4:24; John 6:35; John 8:12; John 8:32
John 8:36; John 10:10; John 10:11; John 10:27; John 11:25; John 13:34; John 13:35; John 14:1; John 14:2; John 14:6
John 14:15; John 14:27; John 15:5; John 15:12; John 15:13; John 16:33; John 20:29
Acts 1:8; Acts 2:38; Acts 4:12; Acts 16:31; Acts 17:28; Acts 20:35
Romans 1:16; Romans 3:23; Romans 5:1; Romans 5:3-4; Romans 5:5; Romans 5:8; Romans 6:23; Romans 8:1; Romans 8:18; Romans 8:28
Romans 8:31; Romans 8:37; Romans 8:38-39; Romans 10:9; Romans 10:17; Romans 12:2; Romans 12:10; Romans 12:12; Romans 12:21; Romans 15:13
1 Corinthians 2:9; 1 Corinthians 10:13; 1 Corinthians 10:31; 1 Corinthians 13:4; 1 Corinthians 13:7; 1 Corinthians 13:13; 1 Corinthians 15:57; 1 Corinthians 15:58; 1 Corinthians 16:13; 1 Corinthians 16:14
2 Corinthians 1:3-4; 2 Corinthians 4:16; 2 Corinthians 4:18; 2 Corinthians 5:7; 2 Corinthians 5:17; 2 Corinthians 9:7; 2 Corinthians 9:8; 2 Corinthians 12:9; 2 Corinthians 13:14
Galatians 2:20; Galatians 5:1; Galatians 5:13; Galatians 5:22-23; Galatians 6:2; Galatians 6:9
Ephesians 2:8-9; Ephesians 2:10; Ephesians 3:20; Ephesians 4:2; Ephesians 4:29; Ephesians 4:32; Ephesians 5:2; Ephesians 6:10; Ephesians 6:11
Philippians 1:6; Philippians 1:21; Philippians 2:3; Philippians 2:13; Philippians 3:13-14; Philippians 4:4; Philippians 4:6; Philippians 4:7; Philippians 4:8; Philippians 4:13
Philippians 4:19
Colossians 1:17; Colossians 2:6-7; Colossians 3:2; Colossians 3:12; Colossians 3:13; Colossians 3:14; Colossians 3:15; Colossians 3:17; Colossians 3:23; Colossians 4:2
1 Thessalonians 5:11; 1 Thessalonians 5:16-18; 1 Thessalonians 5:24
2 Thessalonians 3:3; 2 Thessalonians 3:16
1 Timothy 1:15; 1 Timothy 4:12; 1 Timothy 6:6; 1 Timothy 6:12
2 Timothy 1:7; 2 Timothy 2:15; 2 Timothy 3:16; 2 Timothy 4:7
Titus 3:5
Hebrews 4:12; Hebrews 4:16; Hebrews 10:23; Hebrews 10:24; Hebrews 11:1; Hebrews 11:6; Hebrews 12:1; Hebrews 12:2; Hebrews 13:5; Hebrews 13:8
James 1:2-3; James 1:5; James 1:12; James 1:17; James 1:19; James 1:22; James 4:7; James 4:8; James 4:10; James 5:16
1 Peter 1:3; 1 Peter 2:9; 1 Peter 3:15; 1 Peter 4:8; 1 Peter 4:10; 1 Peter 5:6; 1 Peter 5:7; 1 Peter 5:10
2 Peter 1:3; 2 Peter 3:9; 2 Peter 3:18
1 John 1:9; 1 John 3:1; 1 John 3:18; 1 John 4:4; 1 John 4:7; 1 John 4:8; 1 John 4:16; 1 John 4:18; 1 John 4:19; 1 John 5:14
3 John 1:2
Jude 1:24-25
Revelation 3:20; Revelation 21:4; Revelation 21:5; Revelation 22:13
"""


def fetch(ref):
    url = "https://bible-api.com/" + urllib.parse.quote(ref) + "?translation=kjv"
    req = urllib.request.Request(url, headers={"User-Agent": "ClockHub verse builder"})
    for attempt in range(6):
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 429:  # rate limited: back off and retry
                time.sleep(10 * (attempt + 1))
                continue
            raise
    raise RuntimeError("rate limited too long: " + ref)


def main():
    refs = [r.strip() for line in REFS.strip().splitlines() for r in line.split(";") if r.strip()]
    cache = json.load(open(CACHE, encoding="utf-8")) if os.path.exists(CACHE) else {}
    lines, skipped, failed = [], [], []
    for i, ref in enumerate(refs):
        if ref not in cache:
            try:
                d = fetch(ref)
                cache[ref] = {"reference": d["reference"], "text": d["text"]}
            except Exception as e:  # noqa: BLE001
                failed.append(f"{ref}: {e}")
                continue
            json.dump(cache, open(CACHE, "w", encoding="utf-8"), indent=0, ensure_ascii=False)
            time.sleep(2.2)  # bible-api.com allows about 15 requests per 30 s
            print(f"{i + 1}/{len(refs)} {ref}", flush=True)
        text = re.sub(r"\s+", " ", cache[ref]["text"]).strip()
        if len(text) > MAX_CHARS:
            skipped.append(ref)
            continue
        lines.append(f"{cache[ref]['reference']}\t{text}")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"\nwrote {len(lines)} verses to {OUT}; {len(skipped)} too long, {len(failed)} failed")
    for f in failed:
        print("  failed:", f)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
