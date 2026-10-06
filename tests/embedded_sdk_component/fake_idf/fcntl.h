#pragma once
enum {F_GETFL=1,F_SETFL=2,O_NONBLOCK=4};
extern "C" int fcntl(int,int,...);
