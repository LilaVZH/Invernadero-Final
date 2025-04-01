#include <DHT.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <Wire.h>
#include <RTClib.h>


//Definición de pines 
#define DHTTYPE DHT22
#define PIN_LUZ 2
#define PIN_DHT 4
#define PIN_BOTON 14
#define BUZZER 18
#define PIN_OBSTACULO 19
#define PIN_LED_B 27

DHT dht(PIN_DHT, DHT22);
RTC_DS1307 rtc;


//Constantes de comparación (umbrales)
#define UM_LUZ 500
#define TEMPE 30.0
#define HUME 60.0

//Variables globales 
volatile int counter = 0;
bool taskEnabled = true;
float LUZ = 0, TEMP = 0, HUM = 0;

//Mutex para proteger variables compartidas 
SemaphoreHandle_t tempMutex, humMutex, luzMutex;


QueueHandle_t eventQueue;
volatile unsigned long lastEventTime = 0;
const unsigned long debounceDelay = 200; // Tiempo mínimo entre eventos (ms)


// ISR para el botón y el sensor de movimiento
void IRAM_ATTR eventISR() {
    unsigned long currentTime = millis();
    if (currentTime - lastEventTime > debounceDelay) { // Evita rebotes y lecturas repetidas
        lastEventTime = currentTime;
        int dummy = 0;
        xQueueSendFromISR(eventQueue, &dummy, NULL);
    }
}

// Tarea para manejar la cuenta de eventos (contador)
void TaskContador(void *pvParameters) {
    int dummy;
    while (1) {
        if (xQueueReceive(eventQueue, &dummy, portMAX_DELAY)) {
            counter++;
            Serial.print("Contador: ");
            Serial.println(counter);
        }
    }
}

// Tarea para leer la temperatura
void TaskReadTemp(void *pvParameters) {
    static float tempArray[3] = {0};  
    static int index = 0, validReadings = 0, failedReadings = 0;

    for (;;) {
        if (taskEnabled) {
            float newTemp = dht.readTemperature();
            if (!isnan(newTemp)) {
                tempArray[index] = newTemp;
                index = (index + 1) % 3;
                if (validReadings < 3) validReadings++;
                failedReadings = 0;

                // Tomamos el mutex antes de actualizar TEMP
                if (xSemaphoreTake(tempMutex, portMAX_DELAY)) {
                  TEMP = newTemp;
                  Serial.print("TEMP: "); Serial.println(TEMP);
                  xSemaphoreGive(tempMutex);  // Liberamos el mutex
                }

            } else if (validReadings == 3) {
                TEMP = (tempArray[0] + tempArray[1] + tempArray[2]) / 3.0;
                failedReadings++;
            }
            if (failedReadings >= 3) Serial.println("ERROR: Sensor de temperatura fallando");
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

// Tarea para leer la humedad
void TaskReadHumed(void *pvParameters) {
    static float humArray[3] = {0};  
    static int index = 0, validReadings = 0, failedReadings = 0;

    for (;;) {
        if (taskEnabled) {
            float newHum = dht.readHumidity();
            if (!isnan(newHum)) {
                humArray[index] = newHum;
                index = (index + 1) % 3;
                if (validReadings < 3) validReadings++;
                failedReadings = 0;

                // Tomamos el mutex antes de actualizar HUM
                if (xSemaphoreTake(humMutex, portMAX_DELAY)) {
                  HUM = newHum;
                  Serial.print("HUM: "); Serial.println(HUM);
                  xSemaphoreGive(humMutex);  // Liberamos el mutex
                }
            } else if (validReadings == 3) {
                HUM = (humArray[0] + humArray[1] + humArray[2]) / 3.0;
                failedReadings++;
            }
            if (failedReadings >= 3) Serial.println("ERROR: Sensor de humedad fallando");
        }
        vTaskDelay(pdMS_TO_TICKS(3200));
    }
}

// Tarea para leer la luz
void TaskReadLuz(void *pvParameters) {
    static int luzArray[3] = {0};  
    static int index = 0, validReadings = 0, failedReadings = 0;

    for (;;) {
        if (taskEnabled) {
            int newLuz = analogRead(PIN_LUZ);
            if (newLuz >= 0 && newLuz <= UM_LUZ) {
                luzArray[index] = newLuz;
                index = (index + 1) % 3;
                if (validReadings < 3) validReadings++;
                failedReadings = 0;

                // Tomamos el mutex antes de actualizar LUZ
                if (xSemaphoreTake(luzMutex, portMAX_DELAY)) {
                  LUZ = newLuz;
                  Serial.print("LUZ: "); Serial.println(LUZ);
                  xSemaphoreGive(luzMutex);  // Liberamos el mutex
                }

            } else if (validReadings == 3) {
                LUZ = (luzArray[0] + luzArray[1] + luzArray[2]) / 3.0;
                failedReadings++;
            }
            if (failedReadings >= 3) Serial.println("ERROR: Sensor de luz fallando");
        }
        vTaskDelay(pdMS_TO_TICKS(1600));
    }
}

// Función para calcular un checksum simple
uint8_t calcularChecksum(float temp, float hum, float luz) {
    int sum = (int)temp + (int)hum + (int)luz;
    return sum % 256;  // Tomamos solo 8 bits
}

// Tarea para activar la alarma si se superan los valores umbral
void TaskAlarma(void *pvParameters) {
    for (;;) {
        float luzActual, tempActual, humActual;

        // Tomamos los valores actuales de las variables protegidas con mutex
        if (xSemaphoreTake(luzMutex, portMAX_DELAY)) {
            luzActual = LUZ;
            xSemaphoreGive(luzMutex);
        }

        if (xSemaphoreTake(tempMutex, portMAX_DELAY)) {
            tempActual = TEMP;
            xSemaphoreGive(tempMutex);
        }

        if (xSemaphoreTake(humMutex, portMAX_DELAY)) {
            humActual = HUM;
            xSemaphoreGive(humMutex);
        }

        Serial.print("TEMP2: "); Serial.print(tempActual);
        Serial.print(" HUM2: "); Serial.print(humActual);
        Serial.print(" LUZ2: "); Serial.println(luzActual);

        // Comparación con los umbrales y activación de la alarma
        if (tempActual > TEMPE || humActual > HUME || luzActual > UM_LUZ) {
            tone(BUZZER, 1000);
            digitalWrite(PIN_LED_B, LOW);
        } else {
            noTone(BUZZER);
            digitalWrite(PIN_LED_B, HIGH);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Tarea para verificar los datos, calcular checksum y enviarlos
void TaskProcesarDatos(void *pvParameters) {
    for (;;) {
        float luzD, tempD, humD;
        unsigned long timestamp = millis();
        bool error = false;

        // Tomamos los valores actuales de las variables protegidas con mutex
        if (xSemaphoreTake(luzMutex, portMAX_DELAY)) {
            luzD = LUZ;
            xSemaphoreGive(luzMutex);
        }

        if (xSemaphoreTake(tempMutex, portMAX_DELAY)) {
            tempD = TEMP;
            xSemaphoreGive(tempMutex);
        }

        if (xSemaphoreTake(humMutex, portMAX_DELAY)) {
            humD = HUM;
            xSemaphoreGive(humMutex);
        }

        // Verificamos si algún dato es inválido
        if (isnan(tempD) || isnan(humD) || isnan(luzD)) {
            error = true;
        }

        // Calculamos el checksum
        uint8_t checksum = calcularChecksum(tempD, humD, luzD);

        // Formateo para envío de datos
        Serial.print("{");
        Serial.print("\"TIEMPO MEDICIÓN\": "); Serial.print(timestamp);
        Serial.print(", \"TEMPERATURA\": "); Serial.print(tempD);
        Serial.print(", \"HUMEDAD\": "); Serial.print(humD);
        Serial.print(", \"LUZ\": "); Serial.print(luzD);
        Serial.print(", \"error\": "); Serial.print(error ? "true" : "false");
        Serial.print(", \"checksum\": "); Serial.print(checksum);  // Agregamos el checksum
        Serial.println("}");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
 // Función de checkSleep
void checkSleep() {
    DateTime now = rtc.now();
    if (now.hour() >= 23) { // Si es >= 23:00, entra en Deep Sleep
        Serial.println("Entrando en Deep Sleep...");
        esp_deep_sleep_start();
    }
}


void setup() {

    Serial.begin(9600);
    Wire.begin(21, 22); // Inicializa I2C en SDA 21, SCL 22
       if (!rtc.begin()) {
           Serial.println("No se encontró el RTC");
           while (1);
       }
        if (!rtc.isrunning()) {
            Serial.println("RTC no está en marcha, configurando la hora...");
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // Configura con la hora de compilación
       }

    // Configuración de salidas
    pinMode(PIN_LED_B, OUTPUT);
    pinMode(BUZZER, OUTPUT);


    pinMode(PIN_BOTON, INPUT_PULLUP);
    pinMode(PIN_OBSTACULO, INPUT_PULLUP);
    

    // Configurar interrupciones para el botón y el sensor
    attachInterrupt(PIN_BOTON, eventISR, FALLING);
    attachInterrupt(PIN_OBSTACULO, eventISR, RISING); // Ajusta a FALLING o CHANGE según el sensor

    // Crear la cola en FreeRTOS
    eventQueue = xQueueCreate(10, sizeof(int));

    // Crear los mutex para proteger las variables compartidas
    luzMutex = xSemaphoreCreateMutex();
    tempMutex = xSemaphoreCreateMutex();
    humMutex = xSemaphoreCreateMutex();

    // Crear tareas en FreeRTOS
    xTaskCreate(TaskContador, "TaskContador", 2048, NULL, 1, NULL);
    xTaskCreate(TaskReadTemp, "TaskReadTemp", 2048, NULL, 1, NULL);
    xTaskCreate(TaskReadHumed, "TaskReadHumed", 2048, NULL, 1, NULL);
    xTaskCreate(TaskReadLuz, "TaskReadLuz", 2048, NULL, 1, NULL);
    xTaskCreate(TaskAlarma, "TaskAlarma", 2048, NULL,3, NULL);
    xTaskCreate(TaskProcesarDatos, "TaskProcesarDatos", 2048, NULL, 4 , NULL);
    

    
}

void loop() {
    checkSleep();
    vTaskDelay(pdMS_TO_TICKS(1000));  
}