#include <stdbool.h>
#include "sl_ncp.h"
#include "sl_component_catalog.h"

bool sl_ncp_local_common_evt_process(sl_bt_msg_t *evt)
{
  (void)evt;
  bool pass_evt = true;

  pass_evt &= sl_ncp_local_evt_process(evt);
  return pass_evt;
}

#if defined(SL_CATALOG_BTMESH_PRESENT)

bool sl_ncp_local_common_btmesh_evt_process(sl_btmesh_msg_t *evt)
{
  (void)evt;
  bool pass_evt = true;

  pass_evt &= sl_ncp_local_btmesh_evt_process(evt);
  return pass_evt;
}

#endif // SL_CATALOG_BTMESH_PRESENT