#pragma once

#include <list>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_camera.h"

#define BSP_CAMERA_VFLIP        1
#define BSP_CAMERA_HMIRROR      0
/* I2C */
#define BSP_I2C_SCL           (GPIO_NUM_5)
#define BSP_I2C_SDA           (GPIO_NUM_4)

/* Display */
#define BSP_LCD_SPI_MOSI      (GPIO_NUM_47)
#define BSP_LCD_SPI_CLK       (GPIO_NUM_21)
#define BSP_LCD_SPI_CS        (GPIO_NUM_44)
#define BSP_LCD_DC            (GPIO_NUM_43)
#define BSP_LCD_RST           (GPIO_NUM_NC)
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_46)

/* Camera */
#define BSP_CAMERA_XCLK      (GPIO_NUM_15)
#define BSP_CAMERA_PCLK      (GPIO_NUM_13)
#define BSP_CAMERA_VSYNC     (GPIO_NUM_6)
#define BSP_CAMERA_HSYNC     (GPIO_NUM_7)
#define BSP_CAMERA_D0        (GPIO_NUM_11)
#define BSP_CAMERA_D1        (GPIO_NUM_9)
#define BSP_CAMERA_D2        (GPIO_NUM_8)
#define BSP_CAMERA_D3        (GPIO_NUM_10)
#define BSP_CAMERA_D4        (GPIO_NUM_12)
#define BSP_CAMERA_D5        (GPIO_NUM_18)
#define BSP_CAMERA_D6        (GPIO_NUM_17)
#define BSP_CAMERA_D7        (GPIO_NUM_16)

typedef enum
{
    COMMAND_TIMEOUT = -2,
    COMMAND_NOT_DETECTED = -1,

    MENU_STOP_WORKING = 0,
    MENU_DISPLAY_ONLY = 1,
    MENU_FACE_RECOGNITION = 2,
    MENU_MOTION_DETECTION = 3,

    ACTION_ENROLL = 4,
    ACTION_DELETE = 5,
    ACTION_RECOGNIZE = 6
} command_word_t;

class Observer
{
public:
    virtual void update() = 0;
};

class Subject
{
private:
    std::list<Observer *> observers;

public:
    void attach(Observer *observer)
    {
        this->observers.push_back(observer);
    }

    void detach(Observer *observer)
    {
        this->observers.remove(observer);
    }

    void detach_all()
    {
        this->observers.clear();
    }

    void notify()
    {
        for (auto observer : this->observers)
            observer->update();
    }
};

class Frame
{
public:
    QueueHandle_t queue_i;
    QueueHandle_t queue_o;
    void (*callback)(camera_fb_t *);

    Frame(QueueHandle_t queue_i = nullptr,
          QueueHandle_t queue_o = nullptr,
          void (*callback)(camera_fb_t *) = nullptr) : queue_i(queue_i),
                                                       queue_o(queue_o),
                                                       callback(callback) {}

    void set_io(QueueHandle_t queue_i, QueueHandle_t queue_o)
    {
        this->queue_i = queue_i;
        this->queue_o = queue_o;
    }
};
