#include <ESP8266_AT_Client.h>
#include <util/crc16.h>
#include <avr/eeprom.h>

#define IDLE_TIMEOUT_MS  10000     // Amount of time to wait (in milliseconds) with no data 
                                   // received before closing the connection.  If you know the server
                                   // you're accessing is quick to respond, you can reduce this value.

typedef struct {
  char NETWORK_SSID[32];
  char NETWORK_PASSWORD[32];
  char PUBLIC_URL[256];
  char PUBLIC_KEY[256];
  char PRIVATE_KEY[256];
  char DELETE_KEY[256];
  char ANALOG_ALIAS[8][64];
  uint8_t ANALOG_ENABLE[8];
  uint32_t POST_INTERVAL_SECONDS;
} configuration_t;
configuration_t configuration;
#define CONFIGURATION_EEPROM_BASE_ADDRESS (256)

// the config mode state machine's return values
#define CONFIG_MODE_NOTHING_SPECIAL  (0)
#define CONFIG_MODE_GOT_INIT         (1)
#define CONFIG_MODE_GOT_EXIT         (2)

int esp8266_enable_pin = 23; // Arduino digital the pin that is used to reset/enable the ESP8266 module
Stream * at_command_interface = &Serial1;  // Serial1 is the 'stream' the AT command interface is on

ESP8266_AT_Client esp(esp8266_enable_pin, at_command_interface); // instantiate the client object

#define ESP8266_INPUT_BUFFER_SIZE (1500)
uint8_t input_buffer[ESP8266_INPUT_BUFFER_SIZE] = {0};     // sketch must instantiate a buffer to hold incoming data
                                      // 1500 bytes is way overkill for MQTT, but if you have it, may as well
                                      // make space for a whole TCP packet

void processResponseData(uint8_t * data, uint32_t data_length);
void userConfigurationProcess(void);
uint8_t configModeStateMachine(char b, boolean reset_buffers);

char scratch[2048] = {0};

void get(char * hostname, uint16_t port, char * url_path, void (*responseBodyProcessor)(uint8_t *, uint32_t));

void commitConfiguration(void){
  eeprom_write_block((const void *) &configuration, (void *) CONFIGURATION_EEPROM_BASE_ADDRESS, sizeof(configuration));
}

void commitConfigurationPartial(uint8_t * field_ptr, uint16_t field_size_bytes){  
  uint16_t offset_bytes = (field_ptr - ((uint8_t *) (&configuration)));
  eeprom_write_block((const void *) field_ptr, (void *) (CONFIGURATION_EEPROM_BASE_ADDRESS + offset_bytes), field_size_bytes);
}

unsigned long previousMillis = 0; // will store last time data was posted
long interval = 1000;             // interval at which to post data

char HOSTNAME[256] = "";
char URL_PATH_FORMAT_STRING[1024] = "";
char URL_PATH[2048] = "";

void setup(void){
  Serial.begin(115200);               // debug console
  Serial1.begin(115200);              // AT command interface

  // load the configuration, initialize if necessary
  eeprom_read_block((void *) &configuration, (const void *) CONFIGURATION_EEPROM_BASE_ADDRESS, sizeof(configuration));
  if(configuration.ANALOG_ENABLE[0] == 0xFF){ // we are dealing with virgin EEPROM
    // initialize it to all zeros
    reset("config");
  }
  printConfig();
  userConfigurationProcess();

  // convert interval to milliseconds
  interval = configuration.POST_INTERVAL_SECONDS * 1000;

  // extract the HOSTNAME from the Public URL
  // everything between "http://" and "/"
  char * http_slash_slash = strstr(configuration.PUBLIC_URL, "http://");
  char * ptr = http_slash_slash + strlen("http://");
  if(http_slash_slash != NULL && strlen(ptr) > 8){        
    for(uint16_t ii = 0; ii < 255; ii++){
      if((*ptr) == '/'){        
        URL_PATH_FORMAT_STRING[0] = '/';
        break;
      }
      else{
        HOSTNAME[ii] = *ptr;       
      }
      ptr++;
    }
  }
  //  Serial.print("Hostname: ");
  //  Serial.println(HOSTNAME);

  // build the URL_PATH_FORMAT_STRING
  if(strlen(URL_PATH_FORMAT_STRING) == 1){
    // add input
    strcat(URL_PATH_FORMAT_STRING, "input");

    // add "/[publicKey]?private_key=[privateKey]
    strcat(URL_PATH_FORMAT_STRING, "/");
    strcat(URL_PATH_FORMAT_STRING, configuration.PUBLIC_KEY);
    strcat(URL_PATH_FORMAT_STRING, "?private_key=");
    strcat(URL_PATH_FORMAT_STRING, configuration.PRIVATE_KEY);

    // for each enabled variable add "&[alias]=%d"
    for(uint8_t ii = 0; ii < 8; ii++){
      if(configuration.ANALOG_ENABLE[ii]){
        strcat(URL_PATH_FORMAT_STRING, "&");
        strcat(URL_PATH_FORMAT_STRING, &(configuration.ANALOG_ALIAS[ii][0]));
        strcat(URL_PATH_FORMAT_STRING, "=%d");
      }      
    }
  }
  
  //  Serial.print("Url Path Format String: ");
  //  Serial.println(URL_PATH_FORMAT_STRING);
  
  esp.setInputBuffer(input_buffer, ESP8266_INPUT_BUFFER_SIZE); // connect the input buffer up
  esp.reset();                                                 // reset the module
  
  // Serial.print("Set Mode to Station...");
  esp.setNetworkMode(1);
  // Serial.println("OK");   
  
  Serial.print("Connecting to Network...");  
  esp.connectToNetwork(configuration.NETWORK_SSID, configuration.NETWORK_PASSWORD, 60000, NULL); // connect to the network
  Serial.println("OK");     
}

void sampleAndbuildUrlPathString(){
  uint16_t field[8] = {0};  
  uint8_t num_channels_enabled = 0;    
  memset(URL_PATH, 0, sizeof(URL_PATH));
  for(uint8_t ii = 0; ii < 8; ii++){
    if(configuration.ANALOG_ENABLE[ii]){
      field[num_channels_enabled++] = analogRead(ii);
    }
  }  

  switch(num_channels_enabled){
    case 1: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0]); break;
    case 2: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1]); break;
    case 3: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2]); break;
    case 4: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2], field[3]); break;
    case 5: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2], field[3], field[4]); break;
    case 6: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2], field[3], field[4], field[5]); break;
    case 7: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2], field[3], field[4], field[5], field[6]); break;
    case 8: sprintf(URL_PATH, URL_PATH_FORMAT_STRING, field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7]); break;
  }
    

}

void loop(void){
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    // save the last time you blinked the LED
    previousMillis = currentMillis;    
    sampleAndbuildUrlPathString();
    get(HOSTNAME, 80, URL_PATH, processResponseData);
  }
}

uint16_t download_body_crc16_checksum = 0;
uint32_t download_body_bytes_received = 0;
boolean download_past_header = false; 
uint32_t download_content_length = 0;

void processHeader(char * key, char * value){
  if(strstr(key, "HTTP") != NULL){
    Serial.print(millis());
    Serial.print("::");
    Serial.println(key);
    //Serial.print("\"");
    //Serial.print(key);
    //Serial.print("\" => \"");
    //Serial.print(value);
    //Serial.println("\"");
  }
  
  if(strcmp(key, "Content-Length") == 0){
    download_content_length = strtoul(value, NULL, 10);   
    // Serial.print("Content-Length = ");
    // Serial.println(download_content_length);
  }
}

uint32_t processHeader(uint8_t * data, uint32_t data_length){
  uint32_t start_index = 0;         
  static uint8_t header_guard_index = 0; 
  static boolean past_first_line = false;
  static char key[64] = {0};
  static char value[64] = {0};
  static uint8_t key_or_value = 0;
  static uint8_t keyval_index = 0;
  
  if(!download_past_header){
    for(uint32_t ii = 0; ii < data_length; ii++){                 
      switch(header_guard_index){
      case 0:
        if(data[ii] == '\r') header_guard_index++;
        else if(data[ii] == ':'){
          key_or_value = 1;
          keyval_index = 0;
        }
        else if(past_first_line){
          if(keyval_index < 63){
            if(!((keyval_index == 0) && (data[ii] == ' '))){ // strip leading spaces
              if(key_or_value == 0) key[keyval_index++] = data[ii];
              else value[keyval_index++] = data[ii];
            }
          }
          else{
            // warning the key string doesn't fit in 64 characters
          }
        }
        break;
      case 1:
        if(data[ii] == '\n'){
          header_guard_index++;
          
          if(past_first_line){
            processHeader((char *) key, (char *) value);
          }
          
          past_first_line = true;
          key_or_value = 0;
          keyval_index = 0;
          memset(key, 0, 64);
          memset(value, 0, 64);          
        }
        else header_guard_index = 0;        
        break;
      case 2:
        if(data[ii] == '\r') header_guard_index++;
        else{
          key[keyval_index++] = data[ii];
          header_guard_index = 0;         
        }
        break;
      case 3:
        if(data[ii] == '\n') header_guard_index++;
        else header_guard_index = 0;         
        break;
      case 4:        
        download_past_header = true;
        start_index = ii;
        header_guard_index = 0;
        break;
      }      
    }
  }  

  return start_index;
}

void get(char * hostname, uint16_t port, char * url_path, void (*responseBodyProcessor)(uint8_t *, uint32_t)){      
  unsigned long total_bytes_read = 0;
  uint8_t mybuffer[64] = {0};
  
  // re-initialize the globals
  download_body_crc16_checksum = 0;
  download_body_bytes_received = 0;   
  download_past_header = false;  
  download_content_length = 0;
  
  /* Try connecting to the website.
     Note: HTTP/1.1 protocol is used to keep the server from closing the connection before all data is read.
  */ 
  esp.connect(hostname, port);
  if (esp.connected()) {   
    memset(scratch, 0, 2048);
    snprintf(scratch, 2047, "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n\r\n", url_path, hostname);        
    esp.print(scratch);
  } else {
    Serial.println(F("Error: Server Connection failed"));    
    return;
  }

  //Serial.println(F("Info: -------------------------------------"));
  
  /* Read data until either the connection is closed, or the idle timeout is reached. */ 
  unsigned long lastRead = millis(); 
  unsigned long num_bytes_read = 0;  
  unsigned long start_time = millis();
  uint32_t loop_counter = 0;
  
  while (esp.connected(false) && (millis() - lastRead < IDLE_TIMEOUT_MS)) {        
    while (esp.available()) {
      //char c = esp.read();
      num_bytes_read = esp.read(mybuffer, 64);     
      total_bytes_read += num_bytes_read;
      
      loop_counter++;
      if((loop_counter % 4096) == 0){
        Serial.print(".");
      }

      if(responseBodyProcessor != 0){
        responseBodyProcessor(mybuffer, num_bytes_read); // signal end of stream
      }        
        
      lastRead = millis();
    }
  }
  
  esp.stop();

  //  Serial.println();  
  //  Serial.println("Debug: Response Processed");
  //  Serial.print("Total Bytes: ");
  //  Serial.println(total_bytes_read);
  //  Serial.print("File Size: ");
  //  Serial.println(download_body_bytes_received);
  //  Serial.print("Checksum: ");
  //  Serial.println(download_body_crc16_checksum);  
  //  Serial.print("Duration: ");
  //  Serial.println(millis() - start_time);   
  
  
}

void processResponseData(uint8_t * data, uint32_t data_length){
  uint32_t start_index = processHeader(data, data_length);
  
  if(download_past_header){
    download_body_bytes_received += data_length - start_index;
    for(uint32_t ii = start_index; ii < data_length; ii++){     
      download_body_crc16_checksum = _crc16_update(download_body_crc16_checksum, data[ii]);
    }
  }    
}

const char * command_mode_init_string = "cfg";
void userConfigurationProcess(void){
  uint32_t timeout = 15000; 
  Serial.print("Enter '");
  Serial.print(command_mode_init_string);
  Serial.print("' for CONFIG mode.");
  Serial.println();
  
  Serial.print("Enter 'help' for a list of available commands.");
  Serial.println();
  Serial.print("Enter 'help <command>' for detailed help on any commands.");
  Serial.println();
  
  while(timeout > 0){
    if(Serial.available()){
      if (CONFIG_MODE_GOT_INIT == configModeStateMachine(Serial.read(), false)) {
        prompt();
        break;
      }    
    }
    delay(1);
    if((timeout % 1000) == 0){
      Serial.print(timeout / 1000);
      Serial.print("...");
    }
    timeout--;
  }

  if(timeout > 0){ // user interupted the countdown
    timeout = 5*60000; // 5 minutes
    
    // stay in this loop until we see the exit token
    while(timeout > 0){  
      if(Serial.available()){
        if (CONFIG_MODE_GOT_EXIT == configModeStateMachine(Serial.read(), false)) {
          break;
        }    
      }      
  
      delay(1);
      timeout--;
    }
  }
}


// this state machine receives bytes and
// returns true if the function is in config mode
char * commands[] = {
  "ssid     ",
  "pwd      ",
  "puburl   ",
  "pubkey   ",
  "prikey   ",
  "delkey   ",
  "alias    ",
  "enable   ",
  "disable  ",
  "interval ",
  "reset    ",
  NULL
};

void (*command_functions[])(char * arg) = {
  set_ssid, 
  set_network_password,
  set_public_url,
  set_public_key,
  set_private_key,
  set_delete_key,
  alias, 
  enable,
  disable,
  set_interval,
  reset,
  NULL
};

void lowercase(char * str) {
  uint16_t len = strlen(str);
  if (len < 0xFFFF) {
    for (uint16_t ii = 0; ii < len; ii++) {
      str[ii] = tolower(str[ii]);
    }
  }
}

void get_help_indent(void){
  Serial.print(F("      "));
}

uint8_t configModeStateMachine(char b, boolean reset_buffers) {
  static boolean received_init_code = false;
  const uint8_t buf_max_write_idx = 126; // [127] must always have a null-terminator
  static char buf[128] = {0}; // buffer to hold commands / data
  static uint8_t buf_idx = 0;  // current number of bytes in buf
  boolean line_terminated = false;
  char * first_arg = 0;
  uint8_t ret = CONFIG_MODE_NOTHING_SPECIAL;

  if (reset_buffers) {
    buf_idx = 0;
  }

  // if you are at the last write-able location in the buffer
  // the only legal characters to accept are a backspace, a newline, or a carriage return
  // reject anything else implicitly
  if((buf_idx == buf_max_write_idx) && (b != 0x7F) && (b != 0x0D) && (b != 0x0A)){
    Serial.println(F("Warn: Input buffer full and cannot accept new characters. Press enter to clear buffers."));
  }
  // the following logic rejects all non-printable characters besides 0D, 0A, and 7F
  else if (b == 0x7F) { // backspace key is special
    if (buf_idx > 0) {
      buf_idx--;
      buf[buf_idx] = '\0';
      Serial.print(b); // echo the character
    }
  }
  else if (b == 0x0D || b == 0x0A) { // carriage return or new line is also special
    buf[buf_idx] = '\0'; // force terminator do not advance write pointer
    line_terminated = true;
    Serial.println(); // echo the character
  }
  else if ((buf_idx <= buf_max_write_idx) && isprint(b)) {
    // otherwise if there's space and the character is 'printable' add it to the buffer
    // silently drop all other non-printable characters
    buf[buf_idx++] = b;
    buf[buf_idx] = '\0';
    Serial.print(b); // echo the character
  }

  char lower_buf[128] = {0};
  if (line_terminated) {
    strncpy(lower_buf, buf, 127);
    lowercase(lower_buf);
  }

  // process the data currently stored in the buffer
  if (received_init_code && line_terminated) {
    // with the exeption of the command "exit"
    // commands are always of the form <command> <argument>
    // they are minimally parsed here and delegated to
    // callback functions that take the argument as a string

    // Serial.print("buf = ");
    // Serial.println(buf);
    
    if (strncmp(command_mode_init_string, lower_buf, 3) == 0) {
      ret = CONFIG_MODE_GOT_INIT;
    }
    if (strncmp("exit", lower_buf, 4) == 0) {
      Serial.println(F("Exiting CONFIG mode..."));
      ret = CONFIG_MODE_GOT_EXIT;
    }
    else {
      // the string must have one, and only one, space in it
      uint8_t num_spaces = 0;
      char * p;
      for (p = buf; *p != '\0'; p++) { // all lines are terminated by '\r' above
        if (*p == ' ') {
          num_spaces++;
        }

        if ((num_spaces == 1) && (*p == ' ')) {
          // if this is the first space encountered, null the original string here
          // in order to mark the first argument string
          *p = '\0';
        }
        else if ((num_spaces > 0) && (first_arg == 0) && (*p != ' ')) {
          // if we are beyond the first space,
          // and have not encountered the beginning of the first argument
          // and this character is not a space, it is by definition
          // the beginning of the first argument, so mark it as such
          first_arg = p;
        }
      }

      // deal with commands that can legitimately have no arguments first
      if (strncmp("help", lower_buf, 4) == 0) {
        help_menu(first_arg);
      }
      else if (first_arg != 0) {
        //Serial.print(F("Received Command: \""));
        //Serial.print(buf);
        //Serial.print(F("\" with Argument: \""));
        //Serial.print(first_arg);
        //Serial.print(F("\""));
        //Serial.println();

        // command with argument was received, determine if it's valid
        // and if so, call the appropriate command processing function
        boolean command_found = false;
        for (uint8_t ii = 0; commands[ii] != 0; ii++) {
          if (strncmp(commands[ii], lower_buf, strlen(buf)) == 0) {
            command_functions[ii](first_arg);
            command_found = true;
            break;
          }
        }

        if (!command_found) {
          Serial.print(F("Error: Unknown command \""));
          Serial.print(buf);
          Serial.println(F("\""));
        }
      }
      else if (strlen(buf) > 0) {
        Serial.print(F("Error: Argument expected for command \""));
        Serial.print(buf);
        Serial.println(F("\", but none was received"));
      }
    }
  }
  else if (line_terminated) {
    // before we receive the init code, the only things
    // we are looking for are an exact match to the strings
    // "AQE\r" or "aqe\r"

    if (strncmp(command_mode_init_string, lower_buf, 3) == 0) {
      received_init_code = true;
      ret = CONFIG_MODE_GOT_INIT;
    }
    else if (strlen(buf) > 0) {
      Serial.print(F("Error: Expecting Config Mode Unlock Code ('"));
      Serial.print(command_mode_init_string);
      Serial.print(F("'), but received '"));
      Serial.print(buf);
      Serial.println(F("'"));
    }
  }

  // clean up the buffer if you got a line termination
  if (line_terminated) {
    if (ret == CONFIG_MODE_NOTHING_SPECIAL) {
      prompt();
    }
    buf[0] = '\0';
    buf_idx = 0;
  }

  return ret;
}

void prompt(void) {
  Serial.print(F("CFG>: "));
}

void help_menu(char * arg) {
  const uint8_t commands_per_line = 3;
  const uint8_t first_dynamic_command_index = 2;

  lowercase(arg);

  if (arg == 0) {
    // list the commands that are legal
    Serial.print(F("help    \texit    \t"));
    for (uint8_t ii = 0, jj = first_dynamic_command_index; commands[ii] != 0; ii++, jj++) {
      if ((jj % commands_per_line) == 0) {
        Serial.println();
      }
      //Serial.print(jj + 1);
      //Serial.print(". ");
      Serial.print(commands[ii]);
      Serial.print('\t');
    }
    Serial.println();
  }
  else {
    // we have an argument, so the user is asking for some specific usage instructions
    // as they pertain to this command
    if (strncmp("help", arg, 4) == 0) {
      Serial.println(F("help <param>"));
      Serial.println(F("   <param> is any legal command keyword"));
      Serial.println(F("   result: usage instructions are printed"));
      Serial.println(F("           for the command named <arg>"));
    }
    else if (strncmp("exit", arg, 4) == 0){
      Serial.println(F("exit"));
      get_help_indent(); Serial.println(F("exits CONFIG mode and begins OPERATIONAL mode."));
    }
        else if (strncmp("ssid", arg, 4) == 0){
      Serial.println(F("ssid <string>"));
      get_help_indent(); Serial.println(F("<string> is the SSID of the network the device should connect to."));      
    }
    else if (strncmp("pwd", arg, 3) == 0){
      Serial.println(F("pwd <string>"));
      get_help_indent(); Serial.println(F("<string> is the network password for "));
      get_help_indent(); Serial.println(F("the SSID that the device should connect to."));      
    }
    else if (strncmp("puburl", arg, 6) == 0){
      Serial.println(F("puburl <string>"));
      get_help_indent(); Serial.println(F("<string> is the Public URL provided by data.sparkfun.com"));
      get_help_indent(); Serial.println(F("  e.g. do *not* copy the link location. Use the text on the page (http://...)"));
    }
    else if (strncmp("pubkey", arg, 6) == 0){
      Serial.println(F("pubkey <string>"));
      get_help_indent(); Serial.println(F("<string> is the Public Key provided by data.sparkfun.com"));    
    }
    else if (strncmp("prikey", arg, 6) == 0){
      Serial.println(F("prikey <string>"));
      get_help_indent(); Serial.println(F("<string> is the Private Key provided by data.sparkfun.com"));          
    }
    else if (strncmp("delkey", arg, 6) == 0){
      Serial.println(F("delkey <string>"));
      get_help_indent(); Serial.println(F("<string> is the Delete Key provided by data.sparkfun.com"));          
    }    
    else if (strncmp("alias", arg, 5) == 0){
      Serial.println(F("alias <number> <string>"));
      get_help_indent(); Serial.println(F("<number> is the WildFire Analog Input number 0..7"));          
      get_help_indent(); Serial.println(F("<number> is the associated Field Name on data.sparkfun.com"));          
    }    
    else if (strncmp("enable", arg, 6) == 0){
      Serial.println(F("enable <number>"));
      get_help_indent(); Serial.println(F("<number> is the WildFire Analog Input number 0..7")); 
      get_help_indent(); Serial.println(F("Enabled fields will be published"));
    }
    else if (strncmp("disable", arg, 7) == 0){
      Serial.println(F("disable <number>"));
      get_help_indent(); Serial.println(F("<number> is the WildFire Analog Input number 0..7")); 
      get_help_indent(); Serial.println(F("Disabled fields will not be published"));
    }
    else if (strncmp("interval", arg, 8) == 0){
      Serial.println(F("interval <number>"));
      get_help_indent(); Serial.println(F("<number> number of seconds between posting to data.sparkfun.com"));       
    }
    else if (strncmp("reset", arg, 5) == 0){
      Serial.println(F("reset config"));
      get_help_indent(); Serial.println(F("Clears the configuration data"));       
    }    
  }
}

void set_ssid(char * arg) { 
  // we've reserved 32-bytes of EEPROM for an SSID
  // so the argument's length must be <= 31  
  uint16_t len = strlen(arg);
  if (len < 32) {
    uint8_t * ptr = (uint8_t *) (&(configuration.NETWORK_SSID[0]));
    memset(ptr, 0, 32);
    strncpy((char *) ptr, arg, len);
    commitConfigurationPartial(ptr, 32);
  }
  else {
    Serial.println(F("Error: SSID must be less than 32 characters in length"));
  }
}

void set_network_password(char * arg) {
  // we've reserved 32-bytes of EEPROM for a network password
  // so the argument's length must be <= 31
  uint16_t len = strlen(arg);
  if (len < 32) {
    uint8_t * ptr = (uint8_t *) (&(configuration.NETWORK_PASSWORD[0]));
    memset(ptr, 0, 32);
    strncpy((char *)ptr, arg, len);
    commitConfigurationPartial(ptr, 32);
  }
  else {
    Serial.println(F("Error: Network password must be less than 32 characters in length"));
  }
}

void set_public_url(char * arg) {
  trim_string(arg);
  uint16_t len = strlen(arg);
  if (len < 256) {
    uint8_t * ptr = (uint8_t *) (&(configuration.PUBLIC_URL[0]));
    memset(ptr, 0, 256);
    strncpy((char *)ptr, arg, len);
    commitConfigurationPartial(ptr, 256);
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }  
}

void set_public_key(char * arg) {
  trim_string(arg);
  uint16_t len = strlen(arg);
  if (len < 256) {
    uint8_t * ptr = (uint8_t *) (&(configuration.PUBLIC_KEY[0]));
    memset(ptr, 0, 256);
    strncpy((char *)ptr, arg, len);
    commitConfigurationPartial(ptr, 256);
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }    
}

void set_private_key(char * arg) {
  trim_string(arg);
  uint16_t len = strlen(arg);
  if (len < 256) {
    uint8_t * ptr = (uint8_t *) (&(configuration.PRIVATE_KEY[0]));
    memset(ptr, 0, 256);
    strncpy((char *)ptr, arg, len);
    commitConfigurationPartial(ptr, 256);
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }    
}

void set_delete_key(char * arg) {
  trim_string(arg);
  uint16_t len = strlen(arg);
  if (len < 256) {
    uint8_t * ptr = (uint8_t *) (&(configuration.DELETE_KEY[0]));
    memset(ptr, 0, 256);
    strncpy((char *)ptr, arg, len);
    commitConfigurationPartial(ptr, 256);
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }    
}

void alias(char * arg) { 
  trim_string(arg);  
  char * ch_s = strtok(arg, " ");
  char * alias_s = strtok(NULL, " ");
  char * endPtr;
  uint32_t ch = strtoul(ch_s, &endPtr, 10);
  uint16_t alias_len = strlen(alias_s);
  
  if(endPtr == NULL){
    Serial.print(F("Error: failed to parse channel number: "));
    Serial.println(ch_s);
    return;
  }
  else if(ch > 7){
    Serial.println(F("Error: channel must be in the range 0 .. 7"));    
    return;
  }

  if(alias_len == 0){
    Serial.println(F("Error: No alias string provided"));    
    return;
  }
  
  if (alias_len < 64) {
    uint8_t * ptr = (uint8_t *) (&(configuration.ANALOG_ALIAS[ch][0]));
    memset(ptr, 0, 64);
    strncpy((char *)ptr, alias_s, alias_len);
    commitConfigurationPartial(ptr, 64);

    ptr = (uint8_t *) (&(configuration.ANALOG_ENABLE[ch]));
    *ptr = 1;
    commitConfigurationPartial(ptr, 1);
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }    
}
void enable(char * arg) { 
  trim_string(arg);
  char * ch_s = strtok(arg, " ");
  char * endPtr;
  uint32_t ch = strtoul(ch_s, &endPtr, 10);
  
  if(endPtr == NULL){
    Serial.print(F("Error: failed to parse channel number: "));
    Serial.println(ch_s);
    return;
  }
  else if(ch > 7){
    Serial.println(F("Error: channel must be in the range 0 .. 7"));    
    return;
  }
  else{
    uint8_t * ptr = (uint8_t *) (&(configuration.ANALOG_ENABLE[ch]));
    *ptr = 1;
    commitConfigurationPartial(ptr, 1);
  }
}

void disable(char * arg) { 
  trim_string(arg);
  char * ch_s = strtok(arg, " ");
  char * endPtr;
  uint32_t ch = strtoul(ch_s, &endPtr, 10);
  
  if(endPtr == NULL){
    Serial.print(F("Error: failed to parse channel number: "));
    Serial.println(ch_s);
    return;
  }
  else if(ch > 7){
    Serial.println(F("Error: channel must be in the range 0 .. 7"));    
    return;
  }
  else{
    uint8_t * ptr = (uint8_t *) (&(configuration.ANALOG_ENABLE[ch]));
    *ptr = 0;
    commitConfigurationPartial(ptr, 1);
  } 
}

void set_interval(char * arg){
  trim_string(arg);
  char * num_s = strtok(arg, " ");
  char * endPtr;
  uint32_t num = strtoul(num_s, &endPtr, 10);
  
  if(endPtr == NULL){
    Serial.print(F("Error: failed to parse interval: "));
    Serial.println(num_s);
    return;
  }  
  else if(num < 1){
    Serial.print(F("Error: interval must be >= 1"));
    return;
  } 
  else{
    uint8_t * ptr = (uint8_t *) (&(configuration.POST_INTERVAL_SECONDS));
    configuration.POST_INTERVAL_SECONDS = num;
    commitConfigurationPartial(ptr, 4);
  }   
}

void reset(char * arg) {
  trim_string(arg);  
  lowercase(arg);
  if (strcmp(arg, "config") == 0) {
    memset((void *) &configuration, 0,  sizeof(configuration));
    alias("0 analog0");    // map a0 => "analog0"
    set_interval("10");    // 10 second interval
    commitConfiguration();
  }
  else {
    Serial.println(F("Error: Public URL must be less than 256 characters in length"));
  }    
}
  
void printConfig(void){
  Serial.println("Current Configuration:");
  Serial.println("=======================");
  Serial.println("Network Parameters");
  Serial.println("-----------------------");
  Serial.print("  SSID: ");
  Serial.println(configuration.NETWORK_SSID);
  Serial.print("  Password: ");
  for(uint8_t ii = 0; ii < strlen(configuration.NETWORK_PASSWORD); ii++){
    Serial.print("*");
  }
  
  Serial.println();
  Serial.println("=======================");
  Serial.println("data.sparkfun.com Parameters:");
  Serial.println("-----------------------");
  Serial.print("  Public URL: ");
  Serial.println(configuration.PUBLIC_URL);  
  Serial.print("  Public Key: ");
  Serial.println(configuration.PUBLIC_KEY);  
  Serial.print("  Private Key: ");
  Serial.println(configuration.PRIVATE_KEY);  
  Serial.print("  Delete Key: ");
  Serial.println(configuration.DELETE_KEY);    
  Serial.print("  Sampling Interval: ");
  Serial.print(configuration.POST_INTERVAL_SECONDS);
  Serial.println(" seconds");

  Serial.println();
  Serial.println("=======================");
  Serial.println("Analog Input Aliases:");
  Serial.println("-----------------------");  
  for(uint8_t ii = 0; ii < 8; ii++){
    Serial.print("Analog ");
    Serial.print(ii);
    Serial.print(" => ");
    if(configuration.ANALOG_ENABLE[ii]){      
      Serial.print("[enabled]  ");
    }
    else{
      Serial.print("[disabled] ");
    }    
    Serial.print("'");
    Serial.print(configuration.ANALOG_ALIAS[ii]);
    Serial.print("'");    
    Serial.println();
  }
}

void ltrim_string(char * str){
  uint16_t num_leading_spaces = 0;
  uint16_t len = strlen(str);
  for(uint16_t ii = 0; ii < len; ii++){
    if(!isspace(str[ii])){
      break;      
    }     
    num_leading_spaces++;
  }
  
  if(num_leading_spaces > 0){
    // copy the string left, including the null terminator
    // which is why this loop is <= len
    for(uint16_t ii = 0; ii <= len; ii++){
      str[ii] = str[ii + num_leading_spaces];
    }
  }
}

void rtrim_string(char * str){  
  // starting at the last character in the string
  // overwrite space characters with null characteres
  // until you reach a non-space character
  // or you overwrite the entire string
  int16_t ii = strlen(str) - 1;  
  while(ii >= 0){
    if(isspace(str[ii])){
      str[ii] = '\0';
    }
    else{
      break;
    }
    ii--;
  }
}

void trim_string(char * str){
  ltrim_string(str);  
  rtrim_string(str);
}