#include "sl_component_catalog.h"
#ifdef SL_CATALOG_KERNEL_PRESENT
#include "app_rta_internal.h"
#else // SL_CATALOG_KERNEL_PRESENT
#include "app_rta_internal_bm.h"
#endif // SL_CATALOG_KERNEL_PRESENT
#include "sli_bt_ncp_transport.h"
#include "sli_ncp_sync.h"
#include "sl_ncp.h"

void app_rta_init_contributors(void)
{
  sli_bt_ncp_transport_init();
  sli_ncp_sync_init();
  sl_ncp_init();
}

void app_rta_ready(void)
{
  sli_bt_ncp_transport_rta_ready();
  sl_ncp_rta_ready();
}
