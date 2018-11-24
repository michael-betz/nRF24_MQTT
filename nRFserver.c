#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <bcm2835.h>    //Raspi GPIO library
#include <mosquitto.h>
#include "myNRF24.h"
#include "mySPI_raspi.h"
#include "nRF24L01.h"
//--------------------------------------------------------------------------
// Receive stuff on mosquitto and sent it out over the nRF24 module
// on one of 6 logical channels. Send stuff as string with hex numbers, like:
// `00 01 AB CD`    --> will send 4 bytes
// Will send stuff on address  E0 E7 E7 E7 E7 - E5 E7 E7 E7 E7
//--------------------------------------------------------------------------

uint8_t rxAddrs[5] = { 0xE0, 0xE7, 0xE7, 0xE7, 0xE7 };
char *FIELD = "raw/out/#";          //Stuff sent here will be sent as ACK payloads (on request from target)
char *FIELD_DIR = "raw/outDir/#";   //Stuff sent here will be sent directly (target must be receiving!)

int keepRunning = true;
struct mosquitto *mos = NULL;

void intHandler( int dummy ){
    printf("\n... Shutting down! \n");
    keepRunning = false;
}

void callbackLog(struct mosquitto *mosq, void *obj, int level, const char *str){
    char dateBuffer[100];
    //Output all logging messages which are >= Warning
    if( (level==MOSQ_LOG_WARNING) | (level==MOSQ_LOG_ERR) | (level==MOSQ_LOG_NOTICE) ){
        //Build a time string
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(dateBuffer, sizeof(dateBuffer)-1, "%d.%m.%Y %H:%M:%S", t);
        printf("[%s]  %s\n", dateBuffer, str);
    }
}

uint8_t hexCharToInt( char hexChar ){
    hexChar = tolower( hexChar );
    if( hexChar>='0' && hexChar<='9' ){
        return( hexChar - '0' );
    }
    if( hexChar>='a' && hexChar<='f' ){
        return( hexChar - 'a' + 10 );
    }
    return 0;
}

// Convert a string containing hex digits to a binary buffer, ignores whitespaces, returns length of binary buffer
uint32_t hexStringToBuffer( char *hexString, uint32_t hexStringLen, uint8_t *outBuffer ){
   //Now convert hex values to binary data in buffer
    char currentChar;
    uint32_t i;
    uint8_t hiLoState=0, tempValue, outDataLen=0;
    for( i=0; i<hexStringLen; i++ ){
        currentChar = *hexString++;
        if( currentChar=='\0' ){
            break;                  //End of string, stop!
        }
        if( isxdigit( currentChar ) == false ){
            continue;               //Not a HEX character!
        }
        if( hiLoState == 0 ){        //We got the high byte first
            tempValue = ( hexCharToInt(currentChar) )<<4;
            hiLoState++;
            if( i==hexStringLen-1 ){ //So that if an idiot (me) sends "0E 1" we return the LSB like this [0x0E, 0x01]
                outBuffer[ outDataLen ] = hexCharToInt(currentChar);
                outDataLen++;
            }
        } else if ( hiLoState >= 1 ){//And the low byte second
            tempValue |= ( hexCharToInt(currentChar) );
            hiLoState = 0;
            outBuffer[ outDataLen ] = tempValue;
            outDataLen++;
        }
    }
    return( outDataLen );
}

uint16_t bytesToHex( uint8_t *binaryIn, uint16_t size, uint8_t *stringOut ) {
    const char hex_str[]= "0123456789abcdef";
    uint16_t i;
    uint8_t currentByte;
    for (i=0; i<size; i++){
        currentByte = *binaryIn++;
        *stringOut++ = hex_str[ currentByte >> 4 ];
        *stringOut++ = hex_str[ currentByte & 0x0F ];
        *stringOut++ = ' ';
    }
    stringOut--;
    *stringOut = 0x00;
    return( size * 3 ); 
}

// Executed when a mosquitto message is received
void onMosMessage( struct mosquitto *mosq, void *obj, const struct mosquitto_message *message ){
    uint8_t outBuffer[255], outLen, tempBuffer[255];
    char *topic = message->topic;
    char *payload = message->payload;
    uint32_t len = message->payloadlen;
    if ( len <= 0 ){
        return;
    }
    uint8_t outPipe = topic[strlen(topic)-1] - '0';        //Last char of topic is pipe number (hopefully)
    bool compareResult;
    if( outPipe > 5 ){
        return;
    }
    mosquitto_topic_matches_sub( FIELD, topic, &compareResult );
    if( compareResult ){
        printf("Scheduling Acc payload on pipe %d [%s]\n", outPipe, payload);
        outLen = hexStringToBuffer( payload, len, outBuffer );
        nRfFlush_tx();
        nRfSendAccPayload( outBuffer, outLen, outPipe );
    }
    mosquitto_topic_matches_sub( FIELD_DIR, topic, &compareResult );
    if( compareResult ){
        outLen = hexStringToBuffer( payload, len, outBuffer );
        bytesToHex( outBuffer, outLen, tempBuffer );
        printf("Sending payload on pipe %d [%s] ...", outPipe, tempBuffer);
        rxAddrs[0] = 0xE0 + outPipe;
        nRfSendBytes( outBuffer, outLen, rxAddrs, 0 );
        rxAddrs[0] = 0xE0;
        printf(" done\n");
    }
}

void mosConnect(){
    uint8_t retVal;
    if( mos ){
        mosquitto_disconnect( mos );
        mosquitto_destroy( mos );
        mos = NULL;
    }
    int a,b,c;
    mosquitto_lib_version( &a, &b, &c );
    printf("Mosquitto: V%d.%d.%d Starting up ... \n", a, b, c);
    mos = mosquitto_new( "nRF24_to_MQQT", true, NULL );
    if( !mos ){ printf("Mosquitto: Could not start! Exiting!"); keepRunning=false; }
    mosquitto_log_callback_set( mos, callbackLog );
    mosquitto_reconnect_delay_set( mos, 1, 60, true );
    retVal = mosquitto_connect( mos, "127.0.0.1", 1883, 10 );
    if( retVal!=MOSQ_ERR_SUCCESS ){ printf("Mosquitto: Error connecting to MQTT broker. Exiting!"); keepRunning=false; }
    printf("Mosquitto: Connected!\n");
    retVal = mosquitto_subscribe( mos, NULL, FIELD, 0 );
    if( retVal!=MOSQ_ERR_SUCCESS ){ printf("Mosquitto: Error Subscribing to %s\n", FIELD); return;}
    retVal = mosquitto_subscribe( mos, NULL, FIELD_DIR, 0 );
    if( retVal!=MOSQ_ERR_SUCCESS ){ printf("Mosquitto: Error Subscribing to %s\n", FIELD_DIR); return;}
    mosquitto_message_callback_set( mos, onMosMessage );
    printf("Mosquitto: Subscribed to %s and %s for talkback\n", FIELD, FIELD_DIR);
}

void myInitNrf(){
    nRfInitRX();
    for( uint8_t i=0; i<=5; i++ ){                          //Open all 5 pipes
        rxAddrs[0] = 0xE0+i;
        nRfSetupRXPipe( i, rxAddrs );                       //(only first byte is varied 0xE0 - 0xE5)
    }
    rxAddrs[0] = 0xE0;
    nRfFlush_tx();
    nRfFlush_rx();
    nRfWrite_register( STATUS, (1<<RX_DR) );		        //Clear Data ready flag
    nRfHexdump();
}


int main( int argc, char **argv ){ 
    uint8_t recBuffer[64], msgLen, retVal, rxLen, nPipe;
    char topic[255], message[255], timeString[16];
    time_t tReset = time(NULL);
    signal(SIGINT, intHandler);
    if ( !bcm2835_init() ){
        printf("Could not initialize bcm2835 library. Exiting!\n");
        return -1;
    }
    mosquitto_lib_init();
    mosConnect();
    //mosquitto_loop_start( mos );                            //Start mosquitto daemon thread
    myInitNrf();
    while( keepRunning ){
        time_t tNow = time(NULL);
        strftime(timeString, 16, "%H:%M:%S", localtime(&tNow));
        // Reset nRF module once a day
        if( difftime(tNow,tReset) > 24*60*60 ){
            myInitNrf();
            tReset = tNow;
        }
        if( nRfIsDataReceived() ){
            while( !nRfIsRXempty() && keepRunning ){
                nPipe = nRfgetPipeNo();
                rxLen = nRfGet_RX_Payload_Width();
	            nRfRead_payload( &recBuffer, rxLen );
                sprintf( topic, "raw/%02X", 0xE0+nPipe );   //Form Mosquitto path "raw/E2"
                msgLen = bytesToHex( recBuffer, rxLen, (uint8_t*)message );
                retVal = mosquitto_publish( mos, NULL, topic, msgLen, message, 0, false );
                if( retVal != MOSQ_ERR_SUCCESS ){
                    printf("\n-------------------\n ERROR 0x%02X while Publishing \n----------------------\n", retVal);
                    printf("%s\n", mosquitto_strerror( retVal ) );
                    keepRunning = false;
                }
                printf( "%s %s [%s] ", timeString, topic, message );
            }
            nRfWrite_register( STATUS, (1<<RX_DR) );		//Clear Data ready flag
            printf( "done\n" );
        }
        mosquitto_loop( mos, 300, 1 );
        //bcm2835_delay( 300 );
    }
    printf("\nmosquitto_disconnect() ");
    mosquitto_disconnect( mos );
    //printf("\nmosquitto_loop_stop() ");
    //mosquitto_loop_stop( mos, true );
    printf("\nmosquitto_destroy() ");
    mosquitto_destroy( mos );
    printf("\nbcm2835_spi_end() ");
    bcm2835_spi_end();
    printf("\nbcm2835_close() ");
    bcm2835_close();

    printf("\n\nFinito\n\n");
    return 0;
}

