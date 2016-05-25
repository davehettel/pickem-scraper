#include "pickem.h"
using namespace std;

// This program will fetch a day's games and update the statistics
// for that day. The day is specified as the first command line
// argument for the program, formatted in the same way ESPN's
// website does in their HTTP query strings.

// Compile command: g++ -o pickem-date fetchdate.cpp -I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.9.sdk/usr/include/libxml2 -lxml2 -lcurl -lsqlite3

int main(int argc, char** argv) {

  // Update the refresh table first.
  update_games_table(false, argv[1]);
  
  // Next we've got to download the games to refresh.
  refresh_games();
  
  return 0;
}