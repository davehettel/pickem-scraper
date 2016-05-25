#include "pickem.h"
using namespace std;

// This program will fetch yesterday's games and update the statistics for
// everyone who played.
//
// Compile command: g++ -o pickem-yesterday fetchyesterday.cpp -I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.9.sdk/usr/include/libxml2 -lxml2 -lcurl -lsqlite3

int main() {
  time_t current_time = time(NULL) - 86400;
  struct tm* local_time = localtime(&current_time);
  
  string date = to_string(local_time->tm_year + 1900);
  
  if(local_time->tm_mon <= 8)
    date += "0";
  date += to_string(local_time->tm_mon + 1);
  
  if(local_time->tm_mday <= 9)
    date += "0";
  date += to_string(local_time->tm_mday);
  
  update_games_table(false, date);
  
  refresh_games();
  
  return 0;
}