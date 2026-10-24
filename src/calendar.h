// Calendar events from private iCal (.ics) links (Google, Outlook, iCloud).
// Runs on the network task; keeps the next two weeks of events.
#pragma once

void calendarService();     // refresh every 30 minutes
void calendarRefreshNow();
