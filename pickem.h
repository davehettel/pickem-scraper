#include <curl/curl.h>
#include <sstream>
#include <iostream>
#include <string>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <vector>
#include <sqlite3.h>
#include <sys/stat.h>
#include <regex>
using namespace std;

// Flags to use when compiling:
/*
-I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.9.sdk/usr/include/libxml2 -lxml2 -lcurl -lsqlite3
 */

sqlite3 *database;

// Most of the curl download/file writing code was modified from the cURL examples.
// cURL Copyright (C) 1998 - 2011, Daniel Stenberg, <daniel@haxx.se>, et al.

// Helpful struct for file writing.
struct file_struct {
  const char *filename;
  FILE *stream;
};

// Function to write the downloaded file from memory to the disk.
static size_t my_fwrite(void *buffer, size_t size, size_t nmemb, void *stream)
{
  struct file_struct *out=(struct file_struct *)stream;
  if(out && !out->stream) {
    out->stream=fopen(out->filename, "wb");
    if(!out->stream)
      return -1;
  }
  return fwrite(buffer, size, nmemb, out->stream);
}

// Function to download a file and write it to the disk.
void download(const char* URL, const char* filename)
{
  CURL *curl;
  CURLcode res;
  struct file_struct file = {filename, NULL};
  
  curl_global_init(CURL_GLOBAL_DEFAULT);
  
  curl = curl_easy_init();
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    
    res = curl_easy_perform(curl);
    
    curl_easy_cleanup(curl);
    
    if(CURLE_OK != res)
      fprintf(stderr, "cURL error: %d\n", res);
  }
  
  if(file.stream)
    fclose(file.stream);
  
  curl_global_cleanup();
}

// Convert basically anything to a C++ string. According to cplusplus.com this is
// supposed to be included in the default implementation with the same name, although
// for whatever reason it wasn't in mine, so if you get redefine errors, just comment
// this out I guess?
template <class T>
inline string to_string(const T& t) {
  stringstream stream;
  stream << t;
  return stream.str();
}

// Generic callback function for SQLite. We're only using this when there's
// no alternative.
static int callback(void *NotUsed, int argc, char **argv, char **azColName) {
  return 0;
}

// This function will, by default, go through today's games, fetch the starting time and game ID
// for every game on today, and put them in the "todays_games" table. In the default mode,
// it only needs to be run once per day, probably around 10 AM.
//
// If the command-line argument "date" is provided in the same format that ESPN's scoreboard
// dates are formatted, it will instead fetch the games from that date and update the
// refresh_games table instead. The refresh mode will also be used to download the final
// statistics from the previous night in case there were any post-game changes. This will
// likely happen around the same time.

void update_games_table(bool noremove = false, string date = "today") {
  bool alternate_date = false;
  vector<string> unneeded_files;
  vector<string> game_ids;
  char* error_message = 0;
  
  // If they've called it with "noremove", we won't delete the files at the end. Don't run
  // it like this more than once in a row, because extra game files might get lodged in
  // the wrong spots and won't get deleted unless they're done by hand.
  if(noremove) {
    cout << "Keeping files after the program is complete.\n";
  }
  
  // If somebody's specified a date, we'll download the scoreboard from a specific day
  // instead of today. The dates should be eight character's long, 4 for the year, 2 for
  // the month, and 2 for the day, in that order.
  if(date != "today") {
    alternate_date = true;
    cout << "Fetching the games from " << date << ".\n";
  }
  
  // Open and connect to the database.
  int j = sqlite3_open("pickem.db", &database);
  if(j) {
    cerr << "Can't connect to the database: " << sqlite3_errmsg(database) << "\n";
    return;
  } else
    cout << "Database connected successfully.\n";
  
  // First off, we need to empty the table we're going to reload, since it still contains
  // the games from the previous time this was run.
  if(!alternate_date) {
    j = sqlite3_exec(database, "DELETE FROM todays_games", callback, 0, &error_message);
    if(j) {
      cerr << "SQLite error: " << error_message << "\n";
      sqlite3_free(error_message);
      return;
    } else
      cout << "Today's games table emptied successfully.\n";
  } else {
    j = sqlite3_exec(database, "DELETE FROM refresh_games", callback, 0, &error_message);
    if(j) {
      cerr << "SQLite error: " << error_message << "\n";
      sqlite3_free(error_message);
      return;
    } else
      cout << "Refresh games table emptied successfully.\n";
  }
  
  // Get the date so we can fetch the right games. I'm not sure this will work correctly
  // anywhere other than the east coast, since all of ESPN's starting times are in EST.
  // For now, though, we can just run it on east coast servers.
  time_t current_time = time(NULL);
  struct tm* local_time = localtime(&current_time);
  
  // We need to create a timestamp at noon today so we can grab the correct starting time
  // for every game.
  local_time->tm_hour = 12;
  local_time->tm_min = 0;
  local_time->tm_sec = 0;
  
  // Make the directory structure.
  mkdir("./files", 0755);
  mkdir("./files/previews", 0755);
  
  // Download the schedule to the files directory.
  if(alternate_date) {
    string scoreboard_file = "http://scores.espn.go.com/mlb/scoreboard?date=" + date;
    download(scoreboard_file.c_str(), "./files/scoreboard.html");
  } else
    download("http://scores.espn.go.com/mlb/scoreboard", "./files/scoreboard.html");
  
  unneeded_files.push_back("./files/scoreboard.html");
  
  // Open the scoreboard file and go through it line by line.
  ifstream scoreboard_file("./files/scoreboard.html", ifstream::in);
  if(scoreboard_file.is_open()) {
    string indiv_line;
    
    while(scoreboard_file.good()) {
      getline(scoreboard_file, indiv_line);
      
      // If the line we're looking at has one of the game numbers, we're going to need
      // to log it so we can load that game later.
      if(strstr(indiv_line.c_str(), "var thisGame = new gameObj") != NULL) {
        string match = "1234567890";
        
        // Start trimming from the first digit we find, then, since all the games are
        // 9 digits, just grab the first 9.
        string game_id = strpbrk (indiv_line.c_str(), match.c_str());
        game_id = game_id.substr(0, 9);
        
        // We only need to download the game log right now if we're fetching today's
        // games instead of a refresh.
        if(!alternate_date) {
          
          // Create the locations we're going to use in the download.
          string URL = "http://scores.espn.go.com/mlb/preview?gameId=" + game_id;
          string file = "./files/previews/" + game_id + ".html";
          
          download(URL.c_str(), file.c_str());
          
          // Finally, add the file to the list of files to delete at the end.
          unneeded_files.push_back(file);
        }
        
        // Add the game to the list to check.
        game_ids.push_back(game_id);
      }
    }
  }
  
  // If we chose an alternate date to grab, all we need to do is update the database.
  if(alternate_date) {
    
    // First off, empty the refresh games table.
    j = sqlite3_exec(database, "DELETE FROM refresh_games", callback, 0, &error_message);
    if(j) {
      cerr << "SQLite error: " << error_message << "\n";
      sqlite3_free(error_message);
    } else
      cout << "Refresh games table emptied successfully.\n";
    
    for(int i = 0; i < game_ids.size(); i++) {
      string query = "INSERT INTO refresh_games (game_id) VALUES(" + game_ids[i] + ");";
      
      // Insert the game into the database.
      j = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
      if(j) {
        cerr << "SQLite error: " << error_message << "\n";
        sqlite3_free(error_message);
      }
    }
  }
  
  // If we fetched today's games, we need to iterate through each preview to see when
  // the game begins.
  else {
    for(int i = 0; i < game_ids.size(); i++) {
      string preview_location = "./files/previews/" + game_ids[i] + ".html";
      ifstream game_file(preview_location.c_str(), ifstream::in);
      
      if(game_file.is_open()) {
        string indiv_line;
        
        while(game_file.good()) {
          getline(game_file, indiv_line);
          
          char* line = (char*)indiv_line.c_str();
          
          // We'll go through each line until we get to the one with the starting time.
          // The starting time is in an HTML "p" element with the class of
          // "game-time-location".
          char* position = strstr(line, "game-time-location\"><p>");
          if(position != NULL) {
            
            //  Then we trim the string so just the time is available.
            position += strlen("game-time-location\"><p>");
            position[5] = '\0';
            
            // Then we need to check the starting time and convert it to a Unix timestamp.
            int hour = 12;
            int minute = 0;
            
            // One digit hours.
            if(position[1] == ':') {
              hour += (position[0] - '0');
              minute = (position[2] - '0') * 10 + (position[3] - '0');
            }
            
            // Two digit hours.
            if(position[2] == ':') {
              hour += 10 + (position[1] - '0');
              
              if(hour == 24)
                hour -= 12;
              
              minute = (position[3] - '0') * 10 + (position[4] - '0');
            }
            
            // Build the timestamp with the now correct hour and minute.
            local_time->tm_hour = hour;
            local_time->tm_min = minute;
            time_t start_time = mktime(local_time);
            
            // Build the query.
            string query = "INSERT INTO todays_games (game_id, time) VALUES(" + game_ids[i] + "," + to_string(start_time) + ");";
            
            // Insert the game into the database.
            j = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
            if(j) {
              cerr << "SQLite error: " << error_message << "\n";
              sqlite3_free(error_message);
            }
          }
        }
      }
    }
  }
  
  // Then we'll delete the extra files so they can be replaced next time the program
  // is run. Some of the files will probably already be deleted in other parts of the
  // program, but it won't matter if we delete them twice.
  if(!noremove)
    for(int i = 0; i < unneeded_files.size(); i++)
      remove(unneeded_files[i].c_str());
}

xmlNode* get_next_valid(xmlNode *cur_node) {
  while (cur_node->next)
  {
    cur_node = cur_node->next;
    if(cur_node->type == XML_ELEMENT_NODE && to_string(cur_node->name) == "td" && cur_node->xmlChildrenNode != NULL)
      return cur_node;
  }
}

void insert_game(xmlNode *row, string game_id) {
  xmlNode *cur_node = NULL;
  xmlAttr *cur_attr = NULL;
  xmlChar *value = NULL;
  char* error_message = 0;
  string query = "";
  int db = 0;
  
  string player_id = "";
  int count = 0;
  
  if(&row->properties[0] != NULL)
  {
    for(cur_attr = &row->properties[0]; cur_attr; cur_attr = cur_attr->next) {
      value = xmlGetProp(row, cur_attr->name);
      
      if(to_string(cur_attr->name) == "class") {
        regex expression("(.*)-");
        player_id = regex_replace(to_string(value), expression, "");
      }
    }
  }
  
  for(cur_node = row->children; cur_node; cur_node = cur_node->next)
    if(cur_node->type == XML_ELEMENT_NODE && to_string(cur_node->name) == "td" && cur_node->xmlChildrenNode != NULL)
      count++;
  
  cur_node = row->children;
  if(count == 10) { // Pitcher
    string name = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    string win = "0";
    string save = "0";
    
    if(name.find("(W,") != - 1)
      win = "1";
    
    if(name.find("(S,") != - 1)
      save = "1";
    
    regex expression("\\(.*)\\)");
    name = regex_replace(name, expression, "");
    query = "INSERT OR IGNORE INTO player_id_relation (player_id, name) "
    "VALUES (" + player_id + ", \"" + name + "\"); "
    "UPDATE player_id_relation "
    "SET name=\"" + name + "\" "
    "WHERE player_id=" + player_id + "; ";
    
    db = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
    if(db) {
      cerr << "SQLite error: " << error_message << "\n";
      cout << query << "\n";
      sqlite3_free(error_message);
    }
    
    cur_node = get_next_valid(cur_node);
    string ip = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    int outs = atoi(&ip[0]) * 3 +  atoi(&ip[2]);
    
    cur_node = get_next_valid(cur_node);
    string hits = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string runs = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string walks = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string strikeouts = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    query = "INSERT OR IGNORE INTO pitchers_games (game_id, player_id, W, SV, SO, outs, BB, H, ER) "
    "VALUES (" + game_id + ", "+ player_id + ", "+ win + ", " + save + ", " + strikeouts + ", " + to_string(outs) + ", " + walks + ", " + hits + ", " + runs + ");"
    "UPDATE pitchers_games "
    "SET W=" + win + ", SV=" + save + ", SO=" + strikeouts + ", outs=" + to_string(outs) + ", BB=" + walks + ", H=" + hits + ", ER=" + runs + " "
    "WHERE game_id=" + game_id + " AND player_id=" + player_id + "; ";
    
    db = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
    if(db) {
      cerr << "SQLite error: " << error_message << "\n";
      sqlite3_free(error_message);
    }
    
  } else if (count == 11) { // Hitter
    string name = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    if(name[1] == '-')
      name = name.substr(2);
  
    int last_space = name.find_last_of(" ");
    name = name.substr(0,last_space);
    
    query = "INSERT OR IGNORE INTO player_id_relation (player_id, name) "
    "VALUES (" + player_id + ", \"" + name + "\"); "
    "UPDATE player_id_relation "
    "SET name=\"" + name + "\" "
    "WHERE player_id=" + player_id + "; ";
    
    db = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
    if(db) {
      cerr << "SQLite error: " << error_message << "\n";
      cout << query << "\n";
      sqlite3_free(error_message);
    }
    
    cur_node = get_next_valid(cur_node);
    string at_bats = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string runs = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string hits = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    cur_node = get_next_valid(cur_node);
    string batted_in = to_string((xmlChar*) xmlNodeGetContent(cur_node));
    
    string query = "INSERT OR IGNORE INTO hitters_games (game_id, player_id, H, AB, R, HR, RBI, SB) "
    "VALUES (" + game_id + ", "+ player_id + ", " + hits + ", " + at_bats + ", " + runs + ", 0, " + batted_in + ", 0);"
    "UPDATE hitters_games "
    "SET H=" + hits + ", AB=" + at_bats + ", R=" + runs + ", RBI=" + batted_in + " "
    "WHERE game_id=" + game_id + " AND player_id=" + player_id + "; ";
    
    int db = sqlite3_exec(database, query.c_str(), callback, 0, &error_message);
    if(db) {
      cerr << "SQLite error: " << error_message << "\n";
      sqlite3_free(error_message);
    }
  }
}

// Processes the home run numbers.
void process_hr(string game_id, string information) {
  int cs = information.find("HR:");
  if(cs != -1)
    information = information.substr(cs);
  else return;
  
  cs = information.find("RBI:");
  if(cs != -1)
    information = information.substr(0, cs);
  
  regex expression("\\((.*?)\\)");
  information = regex_replace(information, expression, "");
  cout << information << "\n"; //Placeholder
}

// Processes the stolen base numbers.
void process_sb(string game_id, string information) {
  information = information.substr(11);
  int cs = information.find("CS:");
  if(cs != -1)
    information = information.substr(0, cs);
  
  cs = information.find("SB:");
  if(cs != -1);
    //information = information.substr(4);
  else return;
  
  regex expression("\\((.*?)\\)");
  information = regex_replace(information, expression, "");
  cout << information << "\n"; //Placeholder
}


// Recurses through each element. For now, it should go in the same
// order as the HTML does. All it does at the moment is print the
// content and attributes of each element.
void print_element_names(xmlNode * a_node, string game_id)
{
  xmlNode *cur_node = NULL;
  xmlAttr *cur_attr = NULL;
  xmlChar *value = NULL;
  
  for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
    bool found_player = false;
    if (cur_node->type == XML_ELEMENT_NODE) {
      string name = to_string(cur_node->name);
      //cout << "\nElement name: " << name << "\n";
      
      if(cur_node->xmlChildrenNode != NULL) {
        value = (xmlChar*) xmlNodeGetContent(cur_node);
        string val = to_string(value);
        
        if(name == "td" && val.find("BATTING") != -1)
        {
          process_hr(game_id, val);
          //cout << val << "\n";
        }
        if(name == "td" && val.find("BASERUNNING") != -1)
        {
          process_sb(game_id, val);
        }
      }
      
      if(&cur_node->properties[0] != NULL) {
        for(cur_attr = &cur_node->properties[0]; cur_attr; cur_attr = cur_attr->next) {
          if(cur_attr != NULL)
          {
            value = xmlGetProp(cur_node, cur_attr->name);
            
            string val = to_string(value);
            
            string attr = to_string(cur_attr->name);
            
            if(name == "tr" && attr == "class" && val.find("player-") != -1)
            {
              insert_game(cur_node, game_id);
              found_player = true;
            }
          }
        }
      }
    }
    
    if(!found_player)
      print_element_names(cur_node->children, game_id);
  }
}


// Given a specific game ID and a scoreboard file location, this
// function will update all the statistics in the
void update_statistics(string game_id, string scoreboard_file) {
  
  // First off we have to trim unneeded code from the HTML file so the parser can run through it.
  ifstream game_file(scoreboard_file, ifstream::in);
  ofstream new_file("./files/scores/active.html", ofstream::out);
  
  if(game_file.is_open()) {
    string indiv_line;
    string new_line;
    
    // Make sure it's a valid XML file.
    new_file << "<?xml version=\"1.0\" encoding=\"utf-8\"?><document>";
    
    // Then we'll go through the original line by line.
    while(game_file.good()) {
      getline(game_file, indiv_line);
      
      new_line = indiv_line;
      
      // We only want one specific line. For now, this should
      // only match one line.
      if(new_line.find("<div style=\"width: 435px; float: left;\"><div class=\"mod-container mod-no-header-footer mod-open mod-open-gamepack mod-box\">") != -1) {
        
        // Remove the useless div tags.
        regex expression("<(/?)div(.*?)>");
        new_line = regex_replace(new_line, expression, "");
        
        // Remove the useless spaces.
        expression = "&nbsp;";
        new_line = regex_replace(new_line, expression, "");
        
        // Make the th and thead tags disappear, so all of the
        // tags are the same. We don't need the style information
        // for them either, so that can go too.
        expression = "<(thead|tbody)(.*?)>";
        new_line = regex_replace(new_line, expression, "<tbody>");
        
        expression = "</(thead|tbody)>";
        new_line = regex_replace(new_line, expression, "</tbody>");
        
        expression = "<(th|td)(.*?)>";
        new_line = regex_replace(new_line, expression, "<td>");
        
        expression = "</(th|td)>";
        new_line = regex_replace(new_line, expression, "</td>");
        
        /*expression = "<tr";
        new_line = regex_replace(new_line, expression, "<tr");
        
        expression = "</tr";
        new_line = regex_replace(new_line, expression, "</tr");
        
        expression = "<table";
        new_line = regex_replace(new_line, expression, "<table");
        
        expression = "</table>";
        new_line = regex_replace(new_line, expression, "</table>");*/
        
        // Remove the useless break tags.
        expression = "<(/?)br(.*?)>";
        new_line = regex_replace(new_line, expression, "");
        
        // Fix ESPN's dumbass coding. All attributes need to
        // have quotes around the values, but one - literally
        // one - doesn't.
        expression = "colspan=5";
        new_line = regex_replace(new_line, expression, "colspan=\"5\"");
        
        // Replace the ampersands with proper entities.
        expression = "&";
        new_line = regex_replace(new_line, expression, "&amp;");
        
        // Write the line to the file.
        new_file << new_line;
        
      }
      
    }
  }
  
  // Write the closing tag and close the files.
  new_file << "\n</document>";
  game_file.close();
  new_file.close();
  
  // Now we hand it off to the XML parser. This bit initializes
  // it and loads the file, I guess.
  LIBXML_TEST_VERSION;
  xmlDoc *tree = xmlReadFile("./files/scores/active.html", NULL, 0);
  if (tree == NULL)
    cerr << "Failed to parse ./files/scores/active.html\n";
  
  xmlNode *rootElement = xmlDocGetRootElement(tree);
  
  print_element_names(rootElement, game_id);
  
  // Clean everything up.
  xmlFreeDoc(tree);
  xmlCleanupParser();
  xmlMemoryDump();
}

// This specific callback function for SQLite will download the
// appropriate box score for the record in the DB. Then it'll call
// the function to update the statistics for that individual game.
static int print_games(void *data, int argc, char **argv, char **azColName) {
  string URL = "http://scores.espn.go.com/mlb/boxscore?gameId=" + to_string(argv[0]);
  string file = "./files/scores/" + to_string(argv[0]) + ".html";
  
  download(URL.c_str(), file.c_str());
  
  update_statistics(argv[0], file);
  return 0;
}

// This function will refresh the statistics for every game in the refresh games table.
void refresh_games() {
  char* error_message = 0;
  const char* data = "Callback function called";
  
  // Make the directory structure.
  mkdir("./files", 0755);
  mkdir("./files/scores", 0755);
  
  // Open and connect to the database.
  int j = sqlite3_open("pickem.db", &database);
  if(j) {
    cerr << "Can't connect to the database: " << sqlite3_errmsg(database) << "\n";
    return;
  } else
    cout << "Database connected successfully.\n";
  
  // Now, we'll select each game from the refresh table and use
  // the print_games callback function to download the HTML boxscore
  // from ESPN.
  string query = "SELECT * FROM refresh_games;";
  j = sqlite3_exec(database, query.c_str(), print_games, (void*) data, &error_message);
  if(j) {
    cerr << "SQLite error: " << error_message << "\n";
    sqlite3_free(error_message);
  }
  //sqlite3_close(database);
}