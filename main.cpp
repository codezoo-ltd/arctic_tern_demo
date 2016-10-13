#include "mbed.h"
#include "Websocket.h"

//------------------------------------------------------------------------------------
// You need to configure these cellular modem / SIM parameters.
// These parameters are ignored for LISA-C200 variants and can be left NULL.
//------------------------------------------------------------------------------------
#include "MDM.h"
#include "GPS.h"
//! Set your secret SIM pin here (e.g. "1234"). Check your SIM manual.
#define SIMPIN      NULL
/*! The APN of your network operator SIM, sometimes it is "internet" check your 
  contract with the network operator. You can also try to look-up your settings in 
google: https://www.google.de/search?q=APN+list */
#define APN         "web.sktelecom.com"	/* SK Telecom */
//#define APN         "alwayson.ktfwing.com"	/* KT */
//! Set the user name for your APN, or NULL if not needed
#define USERNAME    NULL
//! Set the password for your APN, or NULL if not needed
#define PASSWORD    NULL
//#define LARGE_DATA
//------------------------------------------------------------------------------------
#include "bq27510_i2c.h"
#include "bq27510.h"
#define MAXSIZE 20
char Rxdata[20];
char Rxbuffer[MAXSIZE];

int LM4F120_bq27510_read(char cmd, unsigned int bytes);
int LM4F120_bq27510_write(char cmd, char data);
unsigned int transBytes2Int(char msb, char lsb);
//------------------------------------------------------------------------------------
#include "x_nucleo_iks01a1.h"

/* Instantiate the expansion board */
static X_NUCLEO_IKS01A1 *mems_expansion_board = X_NUCLEO_IKS01A1::Instance(PB_9, PB_8);

/* Retrieve the composing elements of the expansion board */
static GyroSensor *gyroscope = mems_expansion_board->GetGyroscope();
static MotionSensor *accelerometer = mems_expansion_board->GetAccelerometer();
static MagneticSensor *magnetometer = mems_expansion_board->magnetometer;
static HumiditySensor *humidity_sensor = mems_expansion_board->ht_sensor;
static PressureSensor *pressure_sensor = mems_expansion_board->pt_sensor;
static TempSensor *temp_sensor1 = mems_expansion_board->ht_sensor;
static TempSensor *temp_sensor2 = mems_expansion_board->pt_sensor;

Timer send_timer;
/* Helper function for printing floats & doubles */
static char *printDouble(char* str, double v, int decimalDigits=2)
{
	int i = 1;
	int intPart, fractPart;
	int len;
	char *ptr;

	/* prepare decimal digits multiplicator */
	for (;decimalDigits!=0; i*=10, decimalDigits--);

	/* calculate integer & fractinal parts */
	intPart = (int)v;
	fractPart = (int)((v-(double)(int)v)*i);

	/* fill in integer part */
	sprintf(str, "%i.", intPart);

	/* prepare fill in of fractional part */
	len = strlen(str);
	ptr = &str[len];

	/* fill in leading fractional zeros */
	for (i/=10;i>1; i/=10, ptr++) {
		if(fractPart >= i) break;
		*ptr = '0';
	}

	/* fill in (rest of) fractional part */
	sprintf(ptr, "%i", fractPart);

	return str;
}
//------------------------------------------------------------------------------------

I2C i2c(SDA_FUEL, SCL_FUEL);

DigitalOut myled(LED1);
Serial pc(SERIAL_TX, SERIAL_RX);

int main() {
	pc.baud(115200);

	int ret;

	uint8_t id;
	float value1, value2;
	char buffer1[32], buffer2[32];
	int32_t axes[3];


	unsigned int loopcnt=0;
#ifdef LARGE_DATA
	char buf[2048] = "";
#else
	char buf[512] = "";
#endif
	// Create the GPS object
#if 1   // use GPSI2C class
	GPSI2C gps;
#else   // or GPSSerial class 
	GPSSerial gps; 
#endif

	printf("\r\n--- Starting new run ---\r\n");

	humidity_sensor->ReadID(&id);
	printf("HTS221  humidity & temperature    = 0x%X\r\n", id);
	pressure_sensor->ReadID(&id);
	printf("LPS25H  pressure & temperature    = 0x%X\r\n", id);
	magnetometer->ReadID(&id);
	printf("LIS3MDL magnetometer              = 0x%X\r\n", id);
	gyroscope->ReadID(&id);
	printf("LSM6DS0 accelerometer & gyroscope = 0x%X\r\n", id);


	// Create the modem object
	MDMSerial mdm; // use mdm(D1,D0) if you connect the cellular shield to a C027
	//mdm.setDebug(4); // enable this for debugging issues 
	// initialize the modem 
	MDMParser::DevStatus devStatus = {};
	MDMParser::NetStatus netStatus = {};
	bool mdmOk = mdm.init(SIMPIN, &devStatus);
	mdm.dumpDevStatus(&devStatus);
	if (mdmOk) {
		// wait until we are connected
		mdmOk = mdm.registerNet(&netStatus);
		mdm.dumpNetStatus(&netStatus);
	}
	if (mdmOk)
	{
		// join the internet connection 
		MDMParser::IP ip = mdm.join(APN,USERNAME,PASSWORD);
		if (ip == NOIP)
			printf("Not able to join network");
		else
		{
			mdm.dumpIp(ip);
		}
	}

	char link[128] = "";
	unsigned int i = 0xFFFFFFFF;
	const int wait = 100;
	bool abort = false;

	int temperature, voltage, soc;
	int abs_acc = 0, pre_abs_acc = 0, check_acc = 0;

	double la = 0, lo = 0;
	double a = 0; 
	double s = 0; 
	double d_temperature = 0;
	double d_humidity = 0;


	while(!abort)
	{
		myled = !myled;
		send_timer.start();
		printf("\r\n");
		printf("\r\n");
		printf("\r\n");
		printf("------------------------------------------------------------------\r\n");
		//
		//Read temperature  again(units = 0.1K)
		//
		temperature = 0;
		LM4F120_bq27510_read(bq27510CMD_TEMP_LSB, 2);
		temperature = (transBytes2Int(Rxdata[1], Rxdata[0]))/10 - 273;
		printf("[Fuel] Current Temperature : %d \r\n",temperature);

		//
		//Read voltage (units = mV)
		//
		voltage = 0;
		LM4F120_bq27510_read(bq27510CMD_VOLT_LSB, 2);
		voltage = transBytes2Int(Rxdata[1], Rxdata[0]);
		printf("[Fuel] Current Voltage : %dmV \r\n",voltage);

		//
		//Read state of charge (units = %)
		//
		LM4F120_bq27510_read(bq27510CMD_SOC_LSB, 2);
		soc = transBytes2Int(Rxdata[1], Rxdata[0]);
		printf("[Fuel] State of Charge :%d%%\r\n", soc);

		while ((ret = gps.getMessage(buf, sizeof(buf))) > 0)
		{
			int len = LENGTH(ret);
			if ((PROTOCOL(ret) == GPSParser::NMEA) && (len > 6))
			{
				// talker is $GA=Galileo $GB=Beidou $GL=Glonass $GN=Combined $GP=GPS
				if ((buf[0] == '$') || buf[1] == 'G') {
#define _CHECK_TALKER(s) ((buf[3] == s[0]) && (buf[4] == s[1]) && (buf[5] == s[2]))
					if (_CHECK_TALKER("GLL")) {
//						double la = 0, lo = 0;
						la = lo = 0;
						char ch;
						if (gps.getNmeaAngle(1,buf,len,la) && 
								gps.getNmeaAngle(3,buf,len,lo) && 
								gps.getNmeaItem(6,buf,len,ch) && ch == 'A')
						{
							loopcnt++;
							printf("GPS Location: %.5f %.5f\r\n", la, lo); 
							sprintf(link, "[GPS] I am here! [%ld]\n"
									"https://maps.google.com/?q=%.5f,%.5f",loopcnt, la, lo);
							printf("%s \r\n",link);
						}
					} else if (_CHECK_TALKER("GGA") || _CHECK_TALKER("GNS") ) {
//						double a = 0; 
						a = 0; 
						if (gps.getNmeaItem(9,buf,len,a)) // altitude msl [m]
							printf("GPS Altitude: %.1f\r\n", a); 
					} else if (_CHECK_TALKER("VTG")) {
//						double s = 0; 
						s = 0; 
						if (gps.getNmeaItem(7,buf,len,s)) // speed [km/h]
							printf("GPS Speed: %.1f\r\n", s); 
					}
				}
			}
		}

		temp_sensor1->GetTemperature(&value1);
		humidity_sensor->GetHumidity(&value2);
		printf("[Sensor] HTS221: [temp] %7s°C,   [hum] %s%%\r\n", printDouble(buffer1, value1), printDouble(buffer2, value2));

		d_temperature = value1;
		d_humidity = value2;

		temp_sensor2->GetFahrenheit(&value1);
		pressure_sensor->GetPressure(&value2);
		printf("[Sensor] HLPS25H: [temp] %7s°F, [press] %smbar\r\n", printDouble(buffer1, value1), printDouble(buffer2, value2));

		magnetometer->Get_M_Axes(axes);
		printf("[Sensor] HLIS3MDL [mag/mgauss]:  %6ld, %6ld, %6ld\r\n", axes[0], axes[1], axes[2]);

		accelerometer->Get_X_Axes(axes);
		printf("[Sensor] HLSM6DS0 [acc/mg]:      %6ld, %6ld, %6ld\r\n", axes[0], axes[1], axes[2]);
		
		abs_acc = abs(axes[1]);
		printf("abs acc : %d \r\n",abs_acc);

		gyroscope->Get_G_Axes(axes);
		printf("[Sensor] HLSM6DS0 [gyro/mdps]:   %6ld, %6ld, %6ld\r\n", axes[0], axes[1], axes[2]);

		
		printf("------------------------------------------------------------------\r\n");

#if 1
		if(pre_abs_acc > 20 && abs_acc > 20)
			check_acc++;
		else
			check_acc = 0;

		if(check_acc > 5)
		{
			printf("check_acc : %d, send to Server \r\n",check_acc);
			
			if(send_timer.read() > 10.0f){
				memset(link, 0x0, sizeof(link));

				int socket = mdm.socketSocket(MDMParser::IPPROTO_TCP);
				if (socket >= 0)
				{
					mdm.socketSetBlocking(socket, 10000);
					if (mdm.socketConnect(socket, "175.193.45.163", 56789))
					{
						sprintf(link, "id:%d,lat:%10.6f,long:%11.6f,alti:%f,accu:%4.2f,speed:%f,bearing:%4.2f", 
								soc, la, lo, a, d_temperature, s, d_humidity); 

						mdm.socketSend(socket, link, strlen(link)+1);

						printf("%s\r\n", link);
						mdm.socketClose(socket);
						mdm.socketFree(socket);
					}
					// disconnect  
					//			mdm.disconnect();
				}
				send_timer.reset();
			}
		}
#endif

#ifdef RTOS_H
		Thread::wait(wait);
#else
		::wait(1.0);
#endif			
		pre_abs_acc = abs_acc;
	}
	gps.powerOff();
	mdm.powerOff();
	return 0;
}

/**
 * @brief  Translate two bytes into an integer
 * @param
 * @retval The calculation results
 */
unsigned int transBytes2Int(char msb, char lsb)
{
	unsigned int tmp;

	tmp = ((msb << 8) & 0xFF00);
	return ((unsigned int)(tmp + lsb) & 0x0000FFFF);
}
/**
 * @brief  Receive data from bq27510 through i2c bus
 * @param
 * @retval  0 : read successfully
 *         -1 : read failed
 */
int LM4F120_bq27510_read(char cmd, unsigned int bytes)
{
	char tx[1];
	int ret;

	tx[0] = cmd;
	ret = LM4F120_SWI2CMST_writeBlock(1, 1, tx);
	if(ret < 0){
		printf("LM4F120_SWI2CMST_writeBlock error!!!\r\n");
		return ret;
	}

	ret = LM4F120_SWI2CMST_readBlock(bytes, Rxdata);
	if(ret < 0){
		printf("LM4F120_SWI2CMST_readBlock error!!!\r\n");
		return ret;
	}

	return 0;
}
/**
 * @brief  Send data to bq27510 through i2c bus
 * @param
 * @retval  0 : read successfully
 *         -1 : read failed
 */
int LM4F120_bq27510_write(char cmd, unsigned char data)
{
	char tx[2];
	int ret;

	tx[0] = cmd;
	tx[1] = data;

	ret = LM4F120_SWI2CMST_writeBlock(2, 0, tx);
	if(ret < 0)
		return ret;

	wait_ms(1);

	return 0;
}
