#include "pickem.h"
using namespace std;

// This program will update the today's games table in the database with the
// ESPN game ID and and starting time - in the form of a UNIX timestamp - of
// every game on today.
int main() {
  update_games_table();
  return 0;
}
