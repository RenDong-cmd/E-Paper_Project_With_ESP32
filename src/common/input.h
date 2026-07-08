#pragma once
struct Buttons { bool leftPressed=false, okPressed=false, rightPressed=false; };
void Input_initPins(int pinL, int pinOK, int pinR);
Buttons Input_readEdge();