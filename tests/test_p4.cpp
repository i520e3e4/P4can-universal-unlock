#include <cassert>
#include <cstdio>
#include <vector>
#include "driver/twai.h"
#define CONFIG_IDF_TARGET_ESP32P4 1
constexpr int INPUT=0;
static uint32_t fakeTime=100;
static uint8_t profile=0;
uint32_t millis(){return fakeTime;}
void delay(uint32_t ms){fakeTime+=ms;}
void pinMode(int,int){}
static bool activeProfileIsYl(){return profile==1;}
#include "vehicle_profile.h"
#include "p4_can.h"
#include "can_usb_logger_pure.h"
#include "driver_monitor_capture_pure.h"

static FakeController hw[3];
static twai_general_config_t configs[3];
static int installFail=-1,startFail=-1,uninstallFail=-1,sendBus=-1,sends=0;
static twai_message_t sent;
esp_err_t twai_driver_install_v2(const twai_general_config_t*g,const twai_timing_config_t*t,const twai_filter_config_t*f,twai_handle_t*out){
 assert(g->controller_id>=0&&g->controller_id<3); assert(t->bitrate==500000&&f->acceptAll);
 if(g->controller_id==installFail)return ESP_FAIL;
 configs[g->controller_id]=*g;hw[g->controller_id]={g->controller_id,TWAI_STATE_STOPPED,g->mode,false,{}};*out=&hw[g->controller_id];return ESP_OK;
}
esp_err_t twai_start_v2(twai_handle_t h){if(h->id==startFail)return ESP_FAIL;h->state=TWAI_STATE_RUNNING;return ESP_OK;}
esp_err_t twai_stop_v2(twai_handle_t h){h->state=TWAI_STATE_STOPPED;return ESP_OK;}
esp_err_t twai_driver_uninstall_v2(twai_handle_t h){return h->id==uninstallFail?ESP_FAIL:ESP_OK;}
esp_err_t twai_get_status_info_v2(twai_handle_t h,twai_status_info_t*out){if(!h)return ESP_ERR_INVALID_ARG;out->state=h->state;return ESP_OK;}
esp_err_t twai_receive_v2(twai_handle_t h,twai_message_t*out,TickType_t){if(!h->pending)return ESP_ERR_TIMEOUT;h->pending=false;*out=h->rx;return ESP_OK;}
esp_err_t twai_transmit_v2(twai_handle_t h,const twai_message_t*f,TickType_t){
 if(h->mode==TWAI_MODE_LISTEN_ONLY||h->state!=TWAI_STATE_RUNNING)return ESP_ERR_INVALID_STATE;
 sendBus=h->id;sent=*f;sends++;return ESP_OK;
}
int main(){
 for(profile=1;profile<=5;profile++){
  const auto topology=vehicleProfileDefaultTopology(profile);
  assert(vehicleProfileTopologyValid(profile,topology));
  assert(!vehicleProfileTopologyValid(profile,VEHICLE_TOPOLOGY_RETIRED_PARTY_CHASSIS));
  assert(vehicleProfileNagTorqueSupported(profile,topology));
  assert(vehicleProfileAdvancedEapSupported(profile,topology));
  assert(vehicleProfilePedalMapSupported(profile,topology));
  assert(p4CanBusA()==(profile==1?P4_PARTY:P4_BODY));
  assert(p4CanBusB()==(profile==1?P4_BODY:P4_CHASSIS));
  assert(p4CanBusExtra()==(profile==1?P4_CHASSIS:P4_PARTY));
  for(uint8_t bus=0;bus<3;bus++)assert(p4CanInstall(bus,false));
  const int tx[]={CAN_BODY_TX,CAN_CHASSIS_TX,CAN_PARTY_TX},rx[]={CAN_BODY_RX,CAN_CHASSIS_RX,CAN_PARTY_RX};
  for(int bus=0;bus<3;bus++){assert(configs[bus].tx_io==tx[bus]&&configs[bus].rx_io==rx[bus]);assert(configs[bus].rx_queue_len==256);}
  CanFrame frame{};frame.can_id=0x123;frame.can_dlc=8;frame.data[7]=0x5a;
  p4CanExtraFresh=false;assert(p4CanSendFrame(&frame)!=ESP_OK);
  p4CanExtraFresh=true;assert(p4CanSendFrame(&frame)==ESP_OK&&sendBus==p4CanBusA());
  // Origin survives copy/modification and never follows the default A route.
  for(int bus=0;bus<3;bus++){
   hw[bus].rx={};hw[bus].rx.identifier=0x456;hw[bus].rx.data_length_code=8;hw[bus].rx.data[7]=0x7b;hw[bus].pending=true;
   const uint32_t beforeRx=p4CanRxCount[bus],beforePoll=p4CanRxPolls[bus];
   assert(p4CanReadFrame(bus,&frame)==ESP_OK);
   assert(p4CanRxCount[bus]==beforeRx+1&&p4CanRxPolls[bus]==beforePoll+1&&p4CanLastRxResult[bus]==ESP_OK);
   CanFrame copy=frame;copy.data[0]=9;
   const int before=sends;assert(p4CanSendFrame(&copy)==ESP_OK);assert(sendBus==bus&&sends==before+1&&sent.data[7]==0x7b);
  }
  assert(p4CanReadFrameA(&frame)==ESP_ERR_TIMEOUT);
  assert(p4CanLastRxResult[p4CanBusA()]==ESP_ERR_TIMEOUT);
  twai_status_info_t stats{};stats.state=TWAI_STATE_RUNNING;stats.rx_error_counter=7;
  stats.tx_error_counter=9;stats.msgs_to_rx=4;stats.rx_missed_count=2;
  p4CanPublishStatus(p4CanBusExtra(),stats);
  assert(p4CanRec[p4CanBusExtra()]==7&&p4CanTec[p4CanBusExtra()]==9);
  assert(p4CanRxQueued[p4CanBusExtra()]==4&&p4CanMissed[p4CanBusExtra()]==2);
  assert(p4CanReadFrame(3,&frame)==ESP_ERR_INVALID_ARG);
  hw[2].rx={};hw[2].rx.identifier=0x123;hw[2].rx.data_length_code=9;hw[2].pending=true;
  assert(p4CanReadFrame(2,&frame)==ESP_ERR_INVALID_ARG);
  hw[2].rx.data_length_code=8;hw[2].rx.identifier=0x800;hw[2].pending=true;
  assert(p4CanReadFrame(2,&frame)==ESP_ERR_INVALID_ARG);
  hw[2].rx.identifier=0x123;hw[2].rx.rtr=true;hw[2].rx.data[0]=0xaa;hw[2].pending=true;
  assert(p4CanReadFrame(2,&frame)==ESP_OK&&frame.can_id==0x40000123&&frame.data[0]==0);
  assert(!p4CanReinstall(3,false));
  twai_message_t b{};b.identifier=0x399;b.data_length_code=8;assert(p4CanSendB(&b)==ESP_OK&&sendBus==p4CanBusB());
  frame.bus=P4_PARTY;frame.can_id=0x80000123;assert(p4CanSendFrame(&frame)==ESP_OK&&sent.extd);
  frame.can_id=0x40000123;assert(p4CanSendFrame(&frame)==ESP_OK&&sent.rtr);
  frame.can_id=0x123;frame.can_dlc=9;assert(p4CanSendFrame(&frame)==ESP_ERR_INVALID_ARG);
  frame.can_dlc=8;frame.can_id=0x800;assert(p4CanSendFrame(&frame)==ESP_ERR_INVALID_ARG);
  frame.can_id=0x20000123;assert(p4CanSendFrame(&frame)==ESP_ERR_INVALID_ARG);
  frame.can_id=0x123;frame.bus=3;assert(p4CanSendFrame(&frame)==ESP_ERR_INVALID_STATE);
  frame.bus=2;p4CanOnline[1]=false;assert(p4CanSendFrame(&frame)!=ESP_OK);p4CanOnline[1]=true;
  assert(p4CanStopAll());for(auto h:p4CanHandles)assert(!h);
  for(uint8_t bus=0;bus<3;bus++)assert(p4CanInstall(bus,true));
  const int before=sends;p4CanExtraFresh=true;assert(p4CanSendFrame(&frame)!=ESP_OK&&p4CanSendB(&b)!=ESP_OK&&sends==before);
  assert(p4CanStopAll());
 }
 assert(!vehicleProfileValid(0)&&!vehicleProfileValid(6));
 assert(!vehicleProfileTurnValid(4,2,0)&&vehicleProfileTurnValid(4,2,1)&&vehicleProfileTurnValid(4,2,2));
 installFail=1;assert(!p4CanInstall(1,false)&&!p4CanHandles[1]);installFail=-1;
 startFail=2;assert(!p4CanInstall(2,false)&&!p4CanHandles[2]&&!p4CanOnline[2]);startFail=-1;
 assert(p4CanInstall(2,false));hw[2].state=TWAI_STATE_RECOVERING;assert(!p4CanUninstall(2)&&p4CanHandles[2]);
 const uint32_t before=fakeTime;assert(!p4CanReinstall(2,false)&&fakeTime-before>=1800&&p4CanHandles[2]);
 hw[2].state=TWAI_STATE_BUS_OFF;assert(p4CanReinstall(2,false));
 uninstallFail=2;assert(!p4CanStopAll()&&p4CanHandles[2]&&!p4CanOnline[2]);uninstallFail=-1;assert(p4CanStopAll());
 char line[160];CanUsbFrame f{};f.id=0x123;f.dlc=8;f.data[7]=0xaa;
 for(uint8_t bus=0;bus<3;bus++){f.bus=bus;assert(canUsbFormatFrame(line,sizeof(line),f)>0);char token[12];snprintf(token,sizeof(token),",%u,0,123,8,",bus+1);assert(strstr(line,token));}
 f.bus=3;assert(!canUsbFormatFrame(line,sizeof(line),f));f.bus=2;f.dlc=9;assert(!canUsbFormatFrame(line,sizeof(line),f));
 for(uint8_t bus=0;bus<3;bus++)assert(driver_monitor_capture_pure::targetFrame(bus,0x399));
 assert(!driver_monitor_capture_pure::targetFrame(3,0x399));
 puts("PASS: five profiles, pin maps, origin-preserving TX, three listen-only controllers, invalid frames, partial init/teardown, recovery timeout, USB v2 and third-bus capture");
}
