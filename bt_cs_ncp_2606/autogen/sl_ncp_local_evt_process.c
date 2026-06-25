#include <stdbool.h>
#include "sl_ncp.h"
#include "sl_component_catalog.h"
#include "cs_ras_server_internal.h"
#include "cs_reflector.h"
#include "cs_ras_client_internal.h"
#include "cs_initiator.h"

bool sl_ncp_local_common_evt_process(sl_bt_msg_t *evt)
{
  (void)evt;
  bool pass_evt = true;

  pass_evt &= (cs_ras_server_on_bt_event(evt));

  pass_evt &= (cs_reflector_on_bt_event(evt));

  pass_evt &= (cs_ras_client_on_bt_event(evt));

  pass_evt &= (cs_initiator_on_event(evt));

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