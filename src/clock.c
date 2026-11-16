/* MXSHELL2: the date and time (kernel version 29, <mx/time.h>). The kernel
   keeps the time (UTC, from the PS2's clock); the local time is moved by
   the settings time.zone (minutes east of UTC) and time.dst ("on": an hour
   more). The shell's own: clock.format, "24" or "12". */
#include <mx/debug.h>
#include <mx/time.h>
#include <string.h>

#include "pn.h"

const char *const clock_month[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *const clock_wday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/* the zones, minutes east of UTC, and a place in each (the ones of the
   world's time zones; summer time is time.dst) */
const struct clock_zone clock_zones[] = {
    {-720, "Baker Island"}, {-660, "Samoa"},        {-600, "Hawaii"},      {-570, "Marquesas"},
    {-540, "Alaska"},       {-480, "Los Angeles"},  {-420, "Denver"},      {-360, "Chicago, Mexico City"},
    {-300, "New York"},     {-240, "Halifax"},      {-210, "Newfoundland"}, {-180, "Buenos Aires, Sao Paulo"},
    {-120, "South Georgia"}, {-60, "Azores"},       {0, "London, Lisbon"}, {60, "Amsterdam, Berlin, Paris"},
    {120, "Athens, Cairo"}, {180, "Moscow, Istanbul"}, {210, "Tehran"},   {240, "Dubai"},
    {270, "Kabul"},         {300, "Karachi"},       {330, "India"},        {345, "Nepal"},
    {360, "Dhaka"},         {390, "Yangon"},        {420, "Bangkok, Jakarta"}, {480, "Beijing, Singapore"},
    {525, "Eucla"},         {540, "Tokyo, Seoul"},  {570, "Adelaide"},     {600, "Sydney"},
    {630, "Lord Howe"},     {660, "Solomon Islands"}, {720, "Auckland"},   {765, "Chatham"},
    {780, "Tonga"},         {840, "Kiribati"},
};
const int clock_nzones = (int)(sizeof clock_zones / sizeof clock_zones[0]);

int clock_zone(void) {
  const char *v = cfg_get("time.zone", "0");
  int neg = *v == '-', z = 0;
  if (neg || *v == '+')
    v++;
  while (*v >= '0' && *v <= '9')
    z = z * 10 + (*v++ - '0');
  return neg ? -z : z;
}

int clock_dst(void) { return !strcmp(cfg_get("time.dst", "off"), "on"); }
int clock_24h(void) { return cfg_get("clock.format", "24")[0] != '1'; }

/* days since 1970-01-01 of a date (the proleptic Gregorian calendar) */
static s64 days_of(int y, int m, int d) {
  y -= m <= 2;
  s64 era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int clock_weekday(int y, int m, int d) {
  s64 n = (days_of(y, m, d) + 4) % 7; /* 1970-01-01 was a Thursday */
  return (int)(n < 0 ? n + 7 : n);
}

int clock_mdays(int y, int m) {
  static const u8 n[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : n[m - 1];
}

long long clock_utc_of(const struct mx_tm *local) {
  int off = clock_zone() + (clock_dst() ? 60 : 0);
  return days_of(local->year, local->month, local->day) * 86400 + local->hour * 3600 + local->minute * 60 +
         local->second - (s64)off * 60;
}

int clock_now(struct mx_tm *tm) {
  s64 t = mx_time();
  if (!t)
    return -1;
  mx_localtime(t, tm);
  return 0;
}

void clock_hm(const struct mx_tm *tm, char *s, unsigned n) {
  if (clock_24h())
    mx_snprintf(s, n, "%u:%02u", tm->hour, tm->minute);
  else
    mx_snprintf(s, n, "%u:%02u %s", tm->hour % 12 ? tm->hour % 12 : 12, tm->minute, tm->hour < 12 ? "AM" : "PM");
}

int clock_text(char *s, unsigned n) {
  struct mx_tm tm;
  if (clock_now(&tm) < 0)
    return -1;
  clock_hm(&tm, s, n);
  return 0;
}
