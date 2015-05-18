#include <SPI.h>
#include <dht.h>
#include "printf.h" 
#include "nRF24L01.h"
#include "RF24.h"

const int LED_DIM_PIN = 3; // [P3] "IRQ"
const int CE_PIN = 8;
const int CSN_PIN = 9;
const int LDR_PIN = A0; // P1 A
const int THERM_PIN = A1; // P2 A
const int DHT_PIN = 7;
#define ARRAY_SZ(X) (sizeof(X)/sizeof(X[0]))
#define DIM_MAX 300.0

RF24 radio(CE_PIN, CSN_PIN);
const uint64_t pipe_address = 0xF0F0F0F0F2LL;

dht DHT;

int lamp_off = 0;
int target_light_level = 90;
uint8_t light_level = 150;
uint8_t led_power = 200;
uint8_t old_led_power = 0;
bool thermal_override = 0;

unsigned long next_temperature_check_at;
unsigned long next_lightlevel_send_at;
unsigned long fade_to_black_start_date;
unsigned long next_ambient_temp_report_at;

int percent_to_light_level(int pct)
{
	// light level goes from 4096 to 0 when pct goes from 0 to 100
	// however, the *useful* dimming range is not 4096 to 0, so restrict it to DIM_MAX to 0

	return constrain(DIM_MAX * (100 - pct) / 100.0, 0, 4096);
}

void set_led(uint8_t val)
{
	analogWrite(LED_DIM_PIN, val);
	printf("Set LED to %d%%\n", val*100/255);
}

int get_light_level()
{
	return analogRead(LDR_PIN);
}

void radio_send_led_power(uint8_t event_type)
{
	if (light_level == 0 || light_level == 255 || millis() > next_lightlevel_send_at) {
		radio_send('D', event_type, (led_power * 100 / 255), 0);
		next_lightlevel_send_at = millis() + 500;
	}
}

int change_light_output(int val)
{
	char change = 0;

	int new_power = led_power + val;
	if (new_power < 0) {
		led_power = 0;
	} else if (new_power > 255) {
		led_power = 255;
	} else {
		led_power = new_power;
	}

	if (led_power > old_led_power) {
		change = 'I';
	} else if (led_power < old_led_power) {
		change = 'D';
	}

	if (change) {
		set_led(led_power);
		radio_send_led_power(change);
	}

	old_led_power = led_power;
}

int increase_light_output(int val = 1)
{
	change_light_output(val);
}

int decrease_light_output(int val = 1)
{
	change_light_output(-val);
}

void strobe()
{
	int i = 5;

	while (i--) {
		set_led(0);
		delay(30);
		set_led(led_power);
		delay(30);
	}
}

void stop_lamp()
{
	lamp_off = 1;
	printf("Lamp is now off.\n");
	set_led(0);
	decrease_light_output(255);
	radio_send_light_target();
}

void fadeout()
{
	if (millis() >= fade_to_black_start_date + 15000) {
		stop_lamp();
		fade_to_black_start_date = 0;
		return;
	}
	
	unsigned long remaining = fade_to_black_start_date + 15000 - millis();
	if (remaining < 10000) {
		set_led(led_power * remaining / 10000);
	}
}

int get_temperature()
{
	// Integrated pullup has R1 = 20k
	// We have Uth = Rth * E / (R1 + Rth)
    // let V = Uth * 1023 / E value read by the ADC
    // <=> V = 1023 * Rth / (R1 + Rth)
	// solve in Rth :
    // Rth 	=  R1 * V / (1023 - V)

	// Once we have Rth, use beta parameter equation for NTC:
	// T = Beta / ln (Rth / R0 * exp(-Beta/T0))

	unsigned int adc = analogRead(THERM_PIN);
	const unsigned long int R1 = 21900;
	const float Beta = 4400.0;
	const float Beta_R0 = 100000.0;
	const float Beta_T0 = 25.0+273.0;
	float Rth = R1 * adc / (float)(1023 - adc);

	float T = Beta / log(Rth / (Beta_R0 * exp(-Beta / Beta_T0))) - 273.0;
//	printf("adc is %d, R1*adc is %ld, Rth is %lu, temp is %u\n", adc, R1*adc, (long unsigned int)Rth, (int)(T * 100.0));
	
	return (int)(T * 100.0);
}

void radio_send(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
	uint8_t payload[4] = { p0, p1, p2, p3 };
	radio.stopListening();
	bool ok = radio.write(payload, 4);
	if (ok) 
		Serial.println("send ok");
	else
		Serial.println("send KO");
	radio.startListening();
}

void radio_send_temperature(uint8_t type, int temperature)
{
	radio_send('T', type, temperature & 0xFF, (temperature >> 8) & 0xFF);
}

void radio_send_light_target()
{
	if (lamp_off) {
		radio_send('R', 'O', 'F', 'F');
	} else {
		radio_send('R', '1', target_light_level & 0xFF, (target_light_level >> 8) & 0xFF);
	}
}

void radio_send_ambient(uint8_t deg, uint8_t hum)
{
	radio_send('A', 'N', deg, hum);
}

void setup(){
	printf_begin();
	Serial.begin(57600);

	// Radio init
	radio.begin();
	radio.powerDown();
	radio.setRetries(15, 15);
	radio.setChannel(95);
	radio.setCRCLength(RF24_CRC_16);
	radio.setPayloadSize(sizeof(unsigned long));
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_250KBPS);
 	radio.setAutoAck(true);
	radio.openWritingPipe(pipe_address);
	radio.openReadingPipe(1, pipe_address);
	radio.startListening();

	radio.printDetails();

	// Start LED 
	pinMode(LED_DIM_PIN, OUTPUT);
	set_led(led_power);

	// Light level sensor
	pinMode(LDR_PIN, INPUT);
	digitalWrite(LDR_PIN, HIGH);

	// Thermistor
	pinMode(THERM_PIN, INPUT);
//	digitalWrite(THERM_PIN, LOW);
}

void loop(){

	int light_level = get_light_level();
	if (fade_to_black_start_date) {
		fadeout();
	} else if (!thermal_override && !lamp_off) {
		if (light_level > target_light_level + 5) {
			increase_light_output();
		} else if (light_level < target_light_level - 5) {
			decrease_light_output();
		}
	} 

	if (millis() > next_temperature_check_at) {
		int temp = get_temperature();
		next_temperature_check_at = millis() + 30000;
		if (temp > 8000) {
			printf("Thermal emergency, temp %d.", temp);
			radio_send_temperature('E', temp);
			thermal_override = 1;
			decrease_light_output(255);
		} else if (temp > 6000) {// 60 C 
			printf("Thermal alarm, temp %d.", temp);
			radio_send_temperature('A', temp);
			thermal_override = 1;
			decrease_light_output(25);
			next_temperature_check_at = millis() + 5000;
		} else if (thermal_override && temp < 5500) {
			radio_send_temperature('0', temp);
			printf("End thermal alarm.");
			thermal_override = 0;
		}
	}

	if (millis() > next_ambient_temp_report_at) {
		// Report ambiant temperature & humidity from DHT11
		next_ambient_temp_report_at = millis() + 15L * 60L * 1000L;

		int chk = DHT.read11(DHT_PIN);
		switch (chk) {
			case DHTLIB_OK:
				printf("DHT OK\n");
				break;
			case DHTLIB_ERROR_CHECKSUM: 
				Serial.print("Checksum error,\n"); 
				break;
			case DHTLIB_ERROR_TIMEOUT: 
				Serial.print("Time out error,\n"); 
				break;
			case DHTLIB_ERROR_CONNECT:
				Serial.print("Connect error,\n");
				break;
			case DHTLIB_ERROR_ACK_L:
				Serial.print("Ack Low error,\n");
				break;
			case DHTLIB_ERROR_ACK_H:
				Serial.print("Ack High error,\n");
				break;
			default:
				printf("DHT XXX\n");
		}

		int deg = DHT.temperature;
		int hum = DHT.humidity;
		printf("Ambient temp %d humidity %d%%\n", deg, hum);
		radio_send_ambient(deg, hum);
	}

	while (radio.available()) {
		uint8_t payload[4];
		radio.read(payload, 4);
		switch (payload[0]) {
			case 'L':
				if (payload[1] == 0) {
					stop_lamp();
				} else {
					lamp_off = 0;
					target_light_level = percent_to_light_level(payload[1]);
					printf("Set target light level %d\n", target_light_level);
					radio_send_light_target();
				}
				break;
			case 'Q':
				{
				int light_level = get_light_level();
				radio_send_temperature('N', get_temperature());
				radio_send_light_target();
				radio_send('L', 'N', light_level & 0xFF, (light_level >> 8) & 0xFF);
				radio_send_led_power('N');
				}
				break;
			case 'F':
				printf("Fading out to black...\n");
				fade_to_black_start_date = millis();
				// Notify user that we're shutting down with a brief strobe
				strobe();
				break;
		}
	}
}



