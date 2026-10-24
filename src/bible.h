// Bible verses for the Bible screen, from /bible/verses.txt (King James Version)
#pragma once
#include <Arduino.h>

struct BibleVerse { const char* ref; const char* text; };

int bibleCount();              // loads the file on first use; 0 if missing
BibleVerse bibleGet(int i);
int bibleDailyIndex();         // same verse all day, a different one each day; -1 if the clock isn't set

// Verse of the Day from Bible Gateway, fetched by the net task once a day
struct OnlineVerse { String ref, text, version; int day = -1; };
void bibleService();
void bibleRefreshNow();
bool bibleOnline(OnlineVerse& out);   // true if today's online verse is available
uint32_t bibleOnlineVersion();        // bumps when a new online verse arrives
int bibleToday();                     // local day number, -1 if the clock isn't set
