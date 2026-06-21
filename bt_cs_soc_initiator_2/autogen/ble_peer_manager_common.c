#include "ble_peer_manager_common.h"

#include "app.h"

void ble_peer_manager_on_event(ble_peer_manager_evt_type_t* event)
{
  ble_peer_manager_on_event_initiator(event);

}

