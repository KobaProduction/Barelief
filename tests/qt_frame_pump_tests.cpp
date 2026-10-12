#include <visualization/rendering/qt_frame_pump.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <memory>
using visualization::rendering::QtFramePump;
void require(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
void settle(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    std::vector<QtFramePump::Token> tokens;int projected=0,observed=-1;
    QtFramePump pump([&](auto token){tokens.push_back(token);observed=projected;});pump.setRate(240);
    pump.request();require(tokens.empty(),"request published partial setter state synchronously");
    projected=2;pump.request();settle(1);
    require(tokens.size()==1&&observed==2,"GUI delivery was not coalesced atomically");
    for(int i=0;i<1000;++i)pump.request();
    settle(20);require(tokens.size()==1,"pump outran renderer");
    pump.complete(tokens.front());settle(20);
    require(tokens.size()==2,"latest pending request not delivered");
    pump.setVisible(true);pump.complete(tokens.back());settle(20);
    require(tokens.size()==2,"unchanged visibility manufactured a frame");
    pump.setVisible(false);pump.request();settle(20);
    require(tokens.size()==2,"hidden pump submitted");
    pump.setVisible(true);settle(1);require(tokens.size()==3,"resume lost dirty state");
    const auto stale=tokens.back();pump.setVisible(false);pump.request();pump.setVisible(true);settle(1);
    require(tokens.size()==4,"cancelled frame stranded resumed state");
    pump.request();pump.complete(stale);settle(20);
    require(tokens.size()==4,"stale feedback released current frame");
    pump.complete(tokens.back());settle(20);require(tokens.size()==5,"latest resume request missing");
    pump.complete(tokens.back());settle(20);require(tokens.size()==5,"idle pump repainted");
    std::cout<<"qt_frame_pump_tests PASS\n";
}
