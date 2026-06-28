#include <umockdev.h>
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <string.h>
int main(void){
  UMockdevTestbed *tb = umockdev_testbed_new();
  unsigned char dev[18]={0x12,0x01,0x00,0x02,0,0,0,64,0x34,0x12,0x78,0x56,0x00,0x01,0,0,0,1};
  unsigned char cfg[9]={9,2,18,0,1,1,0,0x80,50};
  unsigned char intf[9]={9,4,0,0,0,0xff,0,0,0};
  unsigned char desc[36]; memcpy(desc,dev,18); memcpy(desc+18,cfg,9); memcpy(desc+27,intf,9);
  /* config wTotalLength = 18 (cfg+intf) */ desc[18+2]=18; desc[18+3]=0;
  char *sp = umockdev_testbed_add_devicev(tb, "usb", "1-1", NULL,
      (char*[]){"idVendor","1234","idProduct","5678","busnum","1","devnum","2",
                "bConfigurationValue","1","bNumConfigurations","1","speed","480",NULL},
      (char*[]){"DEVTYPE","usb_device","SUBSYSTEM","usb","BUSNUM","001","DEVNUM","002",
                "DEVNAME","/dev/bus/usb/001/002","PRODUCT","1234/5678/100",NULL});
  printf("syspath=%s\n", sp?sp:"(null)");
  umockdev_testbed_set_attribute_binary(tb, sp, "descriptors", desc, sizeof desc);

  libusb_context *ctx=NULL; int r=libusb_init(&ctx);
  printf("libusb_init=%d\n", r);
  libusb_device **list; ssize_t n=libusb_get_device_list(ctx,&list);
  printf("devices=%zd\n", n);
  for(ssize_t i=0;i<n;i++){ struct libusb_device_descriptor d;
    if(libusb_get_device_descriptor(list[i],&d)==0)
      printf("  bus=%d addr=%d %04x:%04x class=%d\n",
        libusb_get_bus_number(list[i]),libusb_get_device_address(list[i]),
        d.idVendor,d.idProduct,d.bDeviceClass); }
  if(n>0) libusb_free_device_list(list,1);
  libusb_exit(ctx);
  return 0;
}
