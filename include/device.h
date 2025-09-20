#ifndef INCLUDE_DEVICE_H_
#define INCLUDE_DEVICE_H_

#define DEVICE_COUNT_DEFAULT 256

enum device_types {
    DEV_NULL = 0,
    DEV_CHAR,
    DEV_BLOCK,
};

typedef enum device_types dev_type_t;

struct device_operations {
        void (*write)(char);
        char (*read)(void);
        dev_type_t type;
        char       name[8];
};

typedef struct device_operations dev_op_t;

void     init_device(int device_list_length);
int      device_register(dev_op_t op);
dev_op_t device_find_number(int number);
dev_op_t device_find_name(const char *name);
dev_op_t device_find_type(dev_type_t type);
void     device_write(dev_op_t op, const char data);
char     device_read(dev_op_t op);

#endif
