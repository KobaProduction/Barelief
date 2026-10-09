#include <aggregator/capabilities.h>
#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
QJsonObject c{{"version",3},{"uuid",QUuid::createUuid().toString()},{"control_version",2},{"data_version",4},{"stream_port",8213},{"publisher_incarnation","18446744073709551615"},{"graph_revision","0"},{"streams",QJsonArray{QJsonObject{{"id","count"},{"graph_id","g"},{"node_id",7},{"direction","input"},{"port_index",1},{"payload_version",4}}}}};
require(aggregator_capabilities::valid(c),"canonical directory refused");QJsonObject out;require(aggregator_capabilities::decode(aggregator_capabilities::encode(c),out)&&out==c,"roundtrip");
for(const auto key:{"version","control_version","data_version","stream_port"}){auto bad=c;bad[key]=3.5;out=c;require(!aggregator_capabilities::decode(aggregator_capabilities::encode(bad),out)&&out==c,"fractional value accepted or output changed");}
for(const auto key:{"publisher_incarnation","graph_revision"}){auto bad=c;bad[key]="01";require(!aggregator_capabilities::valid(bad),"noncanonical identity accepted");}
auto bad=c;bad["version"]=2;require(!aggregator_capabilities::valid(bad),"legacy directory accepted");bad=c;bad["data_version"]=3;require(!aggregator_capabilities::valid(bad),"legacy DATA accepted");
auto rows=c["streams"].toArray();auto row=rows[0].toObject();row["id"]="second";rows.append(row);bad=c;bad["streams"]=rows;require(!aggregator_capabilities::valid(bad),"duplicate endpoint accepted");
rows=c["streams"].toArray();row=rows[0].toObject();row["port_index"]=-1;rows[0]=row;bad=c;bad["streams"]=rows;require(!aggregator_capabilities::valid(bad),"negative port accepted");
rows=c["streams"].toArray();row=rows[0].toObject();row["note"]="optional";rows[0]=row;require(aggregator_capabilities::same_stream_bindings(c["streams"].toArray(),rows),"optional annotation changes binding");row["port_index"]=0;rows[0]=row;require(!aggregator_capabilities::same_stream_bindings(c["streams"].toArray(),rows),"port remap ignored");
return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
