WIP Wrapper coming soon!

## Overview
cWebsocket is lightweight websocket server library written in C. This library include functions for easy creating websocket server. It implements [websocket protocol rfc6455](http://tools.ietf.org/html/rfc6455).
This is a fork of the project [here](https://github.com/m8rge/cwebsocket) using ESP32 hardware cryptography.

## Features
Pure C.  
It's tiny!  
It very easy to embed in any your application at any platform.  
Library design was made with microcontrollers architecture in mind.  
MIT Licensed.

## ESP32
With this library you can turn your ESP32 to websocket server and get realtime properties from your microcontroller only with browser!
Uses hardware cryptography from ESP32 for SHA1 and Base64, no software crypto needed!

## Notes
### Not supported
* [secure websocket](http://tools.ietf.org/html/rfc6455#section-3)
* [websocket extensions](http://tools.ietf.org/html/rfc6455#section-9)
* [websocket subprotocols](http://tools.ietf.org/html/rfc6455#section-1.9)
* [status codes](http://tools.ietf.org/html/rfc6455#section-7.4) 
* [cookies and/or authentication-related header fields](http://tools.ietf.org/html/rfc6455#page-19)
* [continuation frame](http://tools.ietf.org/html/rfc6455#section-11.8) (all payload data must be encapsulated into one websocket frame)
* big frames, which payload size bigger than 0xFFFF
 
