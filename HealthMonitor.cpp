#include "HealthMonitor.h"
#include "MAX30101.h"

HealthMonitor::HealthMonitor():
    i2c2(I2C2_SDA, I2C2_SCL),
    spi(SPI0_MOSI, SPI0_MISO, SPI0_SCK, SPI0_SS),
    max14720(&i2c2, MAX14720_I2C_SLAVE_ADDR),
    max30001(&spi),
    max30001_InterruptB(P3_6),
    max30001_Interrupt2B(P4_5),
    max30101_Interrupt(P4_0),
    pwmout(P1_7),
    max30101(&i2c2)
{
    n_ir_buffer_length=500; //buffer length of 100 stores 5 seconds of samples running at 100sps
    spo2_init = false;
}

HealthMonitor::~HealthMonitor(void){}

int HealthMonitor::init() 
{
    int result = init_pmic();
    // set NVIC priorities for GPIO to prevent priority inversion
    NVIC_SetPriority(GPIO_P0_IRQn, 5);
    NVIC_SetPriority(GPIO_P1_IRQn, 5);
    NVIC_SetPriority(GPIO_P2_IRQn, 5);
    NVIC_SetPriority(GPIO_P3_IRQn, 5);
    NVIC_SetPriority(GPIO_P4_IRQn, 5);
    NVIC_SetPriority(GPIO_P5_IRQn, 5);
    NVIC_SetPriority(GPIO_P6_IRQn, 5);
    // used by the MAX30001
    NVIC_SetPriority(SPI1_IRQn, 0);
    result += init_pulse_ox();
    return result;
}

int HealthMonitor::init_pulse_ox() 
{
    MAX30101::ModeConfiguration_u mode_config;
    mode_config.all = 0;
    mode_config.bits.reset = 1;
    int res = max30101.setModeConfiguration(mode_config);
    
    wait_ms(100);
    
    MAX30101::FIFO_Configuration_u fifo_config;
    fifo_config.bits.sample_average = 1;
    fifo_config.bits.fifo_a_full = 17; // fifo almost full = 17
    fifo_config.bits.fifo_roll_over_en = 0; // no roll over
    res = res + max30101.setFIFOConfiguration(fifo_config);

    MAX30101::SpO2Configuration_u spo2_config;
    spo2_config.bits.led_pw = 3; // 411 us
    spo2_config.bits.spo2_sr = 1; // 100 samples per second
    spo2_config.bits.spo2_adc_range = 1; // 4096 nA
    res = res + max30101.setSpO2Configuration(spo2_config);

    MAX30101::InterruptBitField_u interrupt_config;
    interrupt_config.bits.a_full = 1; // Almost full flag
    interrupt_config.bits.ppg_rdy = 1; // New FIFO Data Ready
    max30101.enableInterrupts(interrupt_config);
         

    // ~7 ma for both LED
    res += max30101.setLEDPulseAmplitude(MAX30101::LED1_PA, 0x24);
    res += max30101.setLEDPulseAmplitude(MAX30101::LED2_PA, 0x24);

    mode_config.all = 0;
    mode_config.bits.mode = 0x03;
    res += max30101.setModeConfiguration(mode_config);
    
    return res;
}

void HealthMonitor::spo2_range()
{
    //read the first 500 samples, and determine the signal range
    for(i=0;i<n_ir_buffer_length;i++)
    {
        while(max30101_Interrupt.read()==1);   //wait until the interrupt pin asserts
        
        max30101.read_spo2_fifo((aun_red_buffer+i), (aun_ir_buffer+i));  //read from MAX30102 FIFO
            
    }
    //calculate heart rate and SpO2 after first 500 samples (first 5 seconds of samples)
    maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid); 
    spo2_init = true;    
}

void HealthMonitor::read_spo2(uint32_t *spo2, uint32_t *hr)
{
    if(!spo2_init)
        spo2_range();
    
    i=0;

    //dumping the first 100 sets of samples in the memory and shift the last 400 sets of samples to the top
    for(i=100;i<500;i++)
    {
        aun_red_buffer[i-100]=aun_red_buffer[i];
        aun_ir_buffer[i-100]=aun_ir_buffer[i];
    }

    //take 100 sets of samples before calculating the heart rate.
    for(i=400;i<500;i++)
    {
        while(max30101_Interrupt.read()==1);
        max30101.read_spo2_fifo((aun_red_buffer+i), (aun_ir_buffer+i));

    }
    maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid); 
    //spo2_range();
    printf("red=");
    printf("%i", aun_red_buffer[i]);
    printf(", ir=");
    printf("%i", aun_ir_buffer[i]);
    printf(", HR=%i, ", n_heart_rate); 
    printf("HRvalid=%i, ", ch_hr_valid);
    printf("SpO2=%i, ", n_sp02);
    printf("SPO2Valid=%i\n\r", ch_spo2_valid);
    *spo2 = n_sp02;
    *hr = n_heart_rate;
}

int HealthMonitor::init_pmic() {
    // initialize HVOUT on the MAX14720 PMIC
    int result = max14720.init();
    if (result == MAX14720_ERROR){
        return -1;
    }
    max14720.boostEn = MAX14720::BOOST_ENABLED;
    max14720.boostSetVoltage(HVOUT_VOLTAGE); 
    return result;
}

int HealthMonitor::init_ecg() {
    max30001_InterruptB.disable_irq();
    max30001_Interrupt2B.disable_irq();
    max30001_InterruptB.mode(PullUp);
    max30001_InterruptB.fall(&MAX30001::Mid_IntB_Handler);
    max30001_Interrupt2B.mode(PullUp);
    max30001_Interrupt2B.fall(&MAX30001::Mid_Int2B_Handler);
    max30001_InterruptB.enable_irq();
    max30001_Interrupt2B.enable_irq();
    max30001.AllowInterrupts(1);
    // Configuring the FCLK for the ECG, set to 32.768KHZ
    max30001.FCLK_MaximOnly();    // mbed does not provide the resolution necessary, so for now we have a specific solution...
    max30001.sw_rst(); // Do a software reset of the MAX30001
    max30001.INT_assignment(MAX30001::MAX30001_INT_B,    MAX30001::MAX30001_NO_INT,   MAX30001::MAX30001_NO_INT,  //  en_enint_loc,      en_eovf_loc,   en_fstint_loc,
                                     MAX30001::MAX30001_INT_2B,   MAX30001::MAX30001_INT_2B,   MAX30001::MAX30001_NO_INT,  //  en_dcloffint_loc,  en_bint_loc,   en_bovf_loc,
                                     MAX30001::MAX30001_INT_2B,   MAX30001::MAX30001_INT_2B,   MAX30001::MAX30001_NO_INT,  //  en_bover_loc,      en_bundr_loc,  en_bcgmon_loc,
                                     MAX30001::MAX30001_INT_B,    MAX30001::MAX30001_NO_INT,   MAX30001::MAX30001_NO_INT,  //  en_pint_loc,       en_povf_loc,   en_pedge_loc,
                                     MAX30001::MAX30001_INT_2B,   MAX30001::MAX30001_INT_B,    MAX30001::MAX30001_NO_INT,  //  en_lonint_loc,     en_rrint_loc,  en_samp_loc,
                                     MAX30001::MAX30001_INT_ODNR, MAX30001::MAX30001_INT_ODNR);                            //  intb_Type,         int2b_Type)

    max30001.CAL_InitStart(0b1, 0b1, 0b1, 0b011, 0x7FF, 0b0);
    max30001.ECG_InitStart(0b1, 0b1, 0b1, 0b0, 0b10, 0b11, 0x1F, 0b00,
                               0b00, 0b0, 0b01);
    max30001.PACE_InitStart(0b1, 0b0, 0b0, 0b1, 0x0, 0b0, 0b00, 0b0,
                                  0b0);
    max30001.BIOZ_InitStart(0b1, 0b1, 0b1, 0b10, 0b11, 0b00, 7, 0b0,
                                  0b010, 0b0, 0b10, 0b00, 0b00, 2, 0b0,
                                  0b111, 0b0000);
    max30001.RtoR_InitStart(0b1, 0b0011, 0b1111, 0b00, 0b0011, 0b000001,
                                0b00, 0b000, 0b01);
    max30001.Rbias_FMSTR_Init(0b01, 0b10, 0b1, 0b1, 0b00);
    return 0;  
} 

void HealthMonitor::read_ecg(uint8_t* data) {
    unsigned int i;
    uint8_t *bytePtr;
    MAX30001::max30001_bledata_t heartrateData;
    max30001.ReadHeartrateData(&heartrateData);
    bytePtr = reinterpret_cast<uint8_t *>(&heartrateData);
    for (i = 0; i < sizeof(MAX30001::max30001_bledata_t); i++)
      data[i] = bytePtr[i];
} 

