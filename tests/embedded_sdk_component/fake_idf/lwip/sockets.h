#pragma once
#include <cstddef>
extern "C" int recv(int,void *,std::size_t,int);
extern "C" int send(int,const void *,std::size_t,int);
