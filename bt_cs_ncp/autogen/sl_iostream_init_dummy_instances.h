#ifndef SL_IOSTREAM_INIT_DUMMY_INSTANCES_H
#define SL_IOSTREAM_INIT_DUMMY_INSTANCES_H

#include "sl_iostream.h"
#ifdef __cplusplus
extern "C" {
#endif


extern sl_iostream_t *sl_iostream_dummy_mock_handle;
extern sl_iostream_instance_info_t sl_iostream_instance_dummy_mock_info;


// Initialize only iostream dummy instance(s)
void sl_iostream_dummy_init_instances(void);

#ifdef __cplusplus
}
#endif

#endif // SL_IOSTREAM_INIT_DUMMY_INSTANCES_H