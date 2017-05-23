# rpi-openmax
raspberry pi Openmax IL base application project

started from  http://solitudo.net/software/raspberrypi/rpi-openmax-demos/

The goal is to building a drone camera appliation like this

  (73)camera(70) ---> (250)splitter(251)---->(200)encoder(201) -----> write_media ----> FILE   
      (2K)  (71)                   (252) ---->(60)resize  (61) -----> (200)encoder(201) -----> network   
                                                             (320x240)
                                                             
                                                             
