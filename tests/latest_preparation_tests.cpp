#include <visualization/rendering/latest_preparation.h>
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <cstdlib>
using namespace std::chrono_literals;
using Worker=visualization::rendering::LatestPreparation<int,int>;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
template<class F> void ready(F& future){require(future.wait_for(3s)==std::future_status::ready,"worker deadline exceeded");}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    {
        std::promise<void> started,release,finished;
        auto gate=release.get_future().share();auto begin=started.get_future();auto done=finished.get_future();
        Worker worker([&](int& result,const int& input){if(input==1){started.set_value();gate.wait();}result=input;if(input==3)finished.set_value();});
        worker.submit(1,{});ready(begin);worker.submit(2,{});worker.submit(3,{});release.set_value();ready(done);
        visualization::rendering::preparationPool().waitForDone();
        int result=0;require(worker.take(result)&&result==3,"latest pending/completion not retained");
        require(!worker.take(result),"completion consumed twice");
    }
    {
        std::promise<void> release,firstStarted,secondStarted,thirdFinished;
        auto gate=release.get_future().share();auto first=firstStarted.get_future(),second=secondStarted.get_future(),third=thirdFinished.get_future();
        std::atomic<int> active=0,peak=0;
        std::atomic<bool> stop=false;
        Worker* left=nullptr;Worker* right=nullptr;
        auto prepare=[&](int& result,const int& input,Worker* self,std::promise<void>& started){
            int count=++active;int old=peak.load();while(old<count&&!peak.compare_exchange_weak(old,count)){}
            if(input==1){started.set_value();gate.wait();}
            result=input;if(!stop)self->submit(input+1,{});--active;
        };
        Worker a([&](auto& out,const auto& in){prepare(out,in,left,firstStarted);});left=&a;
        Worker b([&](auto& out,const auto& in){prepare(out,in,right,secondStarted);});right=&b;
        Worker c([&](int& out,const int& in){out=in;stop=true;thirdFinished.set_value();});
        a.submit(1,{});b.submit(1,{});ready(first);ready(second);c.submit(7,{});release.set_value();ready(third);
        visualization::rendering::preparationPool().waitForDone();
        require(peak<=2,"preparation exceeded pool limit");int out=0;require(c.take(out)&&out==7,"third view starved");
    }
    {
        Worker worker([](int& out,const int& in){if(in==1)throw 1;out=in;});
        worker.submit(1,{});visualization::rendering::preparationPool().waitForDone();
        worker.submit(2,[]{throw 2;});visualization::rendering::preparationPool().waitForDone();
        int out=0;require(worker.take(out)&&out==2,"exception stranded worker state");
    }
    {
        std::promise<void> release,startedA,startedB;
        auto gate=release.get_future().share();auto sa=startedA.get_future(),sb=startedB.get_future();
        Worker a([&](int&,const int&){startedA.set_value();gate.wait();});
        Worker b([&](int&,const int&){startedB.set_value();gate.wait();});
        a.submit(1,{});b.submit(1,{});ready(sa);ready(sb);
        std::atomic<int> unwanted=0;
        auto queued=std::make_unique<Worker>([&](int&,const int&){++unwanted;});queued->submit(1,{});
        std::promise<void> retiring;auto retiringStarted=retiring.get_future();
        auto retired=std::async(std::launch::async,[&]{retiring.set_value();queued.reset();});ready(retiringStarted);
        release.set_value();ready(retired);
        // Closing may race runnable startup; either finishes safely, no borrowed input.
        require(unwanted<=1,"queued teardown executed duplicate job");
    }
    std::cout<<"latest_preparation_tests PASS\n";
}
