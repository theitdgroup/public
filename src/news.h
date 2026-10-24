// News headlines from any RSS/Atom feed plus Google News topic searches.
#pragma once
#include <Arduino.h>

void newsService();        // called from the network task; refreshes on schedule
void newsRefreshNow();     // force a refresh on the next service call
