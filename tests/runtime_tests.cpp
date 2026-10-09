#include <core/latest_delivery.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <thread>
#include <memory>
#include <stdexcept>
#include <cstdio>
static void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
static void pump(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{
 int delivered=0,newest=-1;auto delivery=std::make_shared<LatestDelivery<int>>(&app,[&](int value){++delivered;newest=value;});
 std::thread producer([&]{for(int i=0;i<10000;++i)delivery->publish(i);});producer.join();
 require(delivered==0,"callback ran outside GUI event loop");pump(5);require(delivered==1&&newest==9999,"unbounded queue or lost latest snapshot");
 auto* receiver=new QObject;int cancelled=0;auto removed=std::make_shared<LatestDelivery<int>>(receiver,[&](int){++cancelled;});
 removed->publish(1);removed->cancel();delete receiver;std::thread late([&]{for(int i=0;i<10000;++i)removed->publish(i);});late.join();pump(5);require(cancelled==0,"cancelled delivery used destroyed receiver");
 int count=0,latest=-1;auto polled=std::make_shared<LatestDelivery<int>>(&app,[&](int value){++count;latest=value;});polled->start_polling();
 std::thread polling([&]{for(int i=0;i<10000;++i)polled->publish(i);});polling.join();require(count==0,"polling producer invoked GUI callback");pump(40);require(count==1&&latest==9999,"polling replayed backlog or lost latest");
 polled->publish(10000);polled->cancel();std::thread after([&]{for(int i=0;i<10000;++i)polled->publish(i);});after.join();pump(40);require(count==1,"polling delivered after cancellation");
 QObject timerOwner;{auto replaced=std::make_shared<LatestDelivery<int>>(&timerOwner,[](int){});replaced->start_polling();}pump(40);require(timerOwner.findChildren<QTimer*>().isEmpty(),"retired delivery retained active polling timer");
 std::puts("current LatestDelivery coalescing/cancel/poll/timer lifetime PASS");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
