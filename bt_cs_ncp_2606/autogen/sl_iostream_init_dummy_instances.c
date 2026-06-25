#include "sl_iostream.h"

// Instance(s) handle and context variable 

sl_iostream_t *sl_iostream_dummy_mock_handle;
static sl_iostream_t dummy_mock;
sl_iostream_t *sl_iostream_dummy_mock_handle = &dummy_mock;
sl_iostream_instance_info_t sl_iostream_instance_dummy_mock_info = {
  .handle = &dummy_mock,
  .name = "dummy_mock",
  .type = SL_IOSTREAM_TYPE_UNDEFINED,
  .periph_id = 0,
};


void sl_iostream_dummy_init_instances(void)
{
  // Instantiate dummy instance(s)
  
  dummy_mock.write = NULL;
  dummy_mock.read = NULL;
  dummy_mock.context = NULL;
  
}
