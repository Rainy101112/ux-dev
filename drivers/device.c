#include "device.h"
#include "alloc.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"

int       device_max = DEVICE_COUNT_DEFAULT;
dev_op_t *device_list;

dev_op_t dummy = {
    .write = NULL,
    .read  = NULL,
    .type  = DEV_NULL,
    .name  = "null",
};

void init_device(int device_list_length)
{
    if (device_list_length > 0) { device_max = device_list_length; }

    device_list = (dev_op_t *)malloc(sizeof(dev_op_t) * device_max);
    for (int i = 0; i < device_max; i++) { device_list[i] = dummy; }
}

int device_register(dev_op_t op)
{
    for (int i = 0; i < device_max; i++) {
        if (device_list[i].type == DEV_NULL) {
            device_list[i] = op;
            return i;
        }
    }
    return -1;
}

dev_op_t device_find_number(int number)
{
    if (number >= 0 && number < device_max) { return device_list[number]; }
    return dummy;
}

dev_op_t device_find_name(const char *name)
{
    for (int i = 0; i < device_max; i++) {
        if (strncmp(device_list[i].name, name, sizeof(device_list[i].name)) == 0) { return device_list[i]; }
    }
    return dummy;
}

dev_op_t device_find_type(dev_type_t type)
{
    for (int i = 0; i < device_max; i++) {
        if (device_list[i].type == type) {
            return device_list[i];
        } else
            continue;
    }

    return dummy;
}

void device_write(dev_op_t op, const char data)
{
    if (op.write != NULL) { op.write(data); }
}

char device_read(dev_op_t op)
{
    if (op.read != NULL) { return op.read(); }
    return '\0';
}
