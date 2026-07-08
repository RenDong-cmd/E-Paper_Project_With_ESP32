#include <Arduino.h>
#include "input.h"
static int PIN_L=32, PIN_OK=33, PIN_R=25;
static uint8_t lastL=1,lastO=1,lastR=1;
void Input_initPins(int pinL, int pinOK, int pinR){
  PIN_L=pinL; PIN_OK=pinOK; PIN_R=pinR;
  pinMode(PIN_L,INPUT_PULLUP); pinMode(PIN_OK,INPUT_PULLUP); pinMode(PIN_R,INPUT_PULLUP);
}
Buttons Input_readEdge(){
  Buttons b; uint8_t L=digitalRead(PIN_L), O=digitalRead(PIN_OK), R=digitalRead(PIN_R);
  b.leftPressed=(lastL==1 && L==0); b.okPressed=(lastO==1 && O==0); b.rightPressed=(lastR==1 && R==0);
  lastL=L; lastO=O; lastR=R; return b;
}