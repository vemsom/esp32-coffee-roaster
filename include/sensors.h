#pragma once

struct SensorReading {
  float bt;
    float et;
      bool btFault;
        bool etFault;
        };

        void sensors_init();
        SensorReading sensors_read();
        bool sensors_safety_triggered();
        
