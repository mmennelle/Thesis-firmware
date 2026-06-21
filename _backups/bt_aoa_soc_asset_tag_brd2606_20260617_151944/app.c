/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "app_assert.h"
#include "app_log.h"
#include "sl_bluetooth.h"
#include "sl_main_init.h"

// CS reflector POC: the same connectable advertising set carries the CTE
// service for the AoA locator AND the RAS service (auto-contributed by
// `cs_ras_server`) for a CS initiator. When any peer connects we spin up a
// reflector instance for that connection; the CS handshake will simply
// never start if the peer is the AoA locator, and the reflector instance
// goes away on disconnect.
#include "cs_antenna.h"
#include "cs_reflector.h"
#include "cs_reflector_config.h"
#include "cs_sync_antenna.h"

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;

// Number of active connections.
static uint8_t connection_count = 0;

// Reflector defaults mirror the standalone CS reflector example.
static cs_reflector_config_t cs_reflector_config = {
  .max_tx_power_dbm = CS_REFLECTOR_MAX_TX_POWER_DBM,
  .cs_sync_antenna  = CS_REFLECTOR_CS_SYNC_ANTENNA
};

/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
void app_process_action(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      // Allow the CS reflector to drive its max TX power range.
      {
        int16_t min_tx_power_x10 = CS_REFLECTOR_MIN_TX_POWER_DBM * 10;
        int16_t max_tx_power_x10 = CS_REFLECTOR_MAX_TX_POWER_DBM * 10;
        (void)sl_bt_system_set_tx_power(min_tx_power_x10,
                                        max_tx_power_x10,
                                        &min_tx_power_x10,
                                        &max_tx_power_x10);
      }

      // Apply the wireless/wired CS antenna offset table for brd2606.
      (void)cs_antenna_configure(CS_REFLECTOR_ANTENNA_OFFSET);

      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      app_assert_status(sc);

      // Generate data for advertising.
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        160, // min. adv. interval (milliseconds * 1.6)
        160, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events
      app_assert_status(sc);

      // Enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);

      app_log_info("[APP] AoA tag + CS reflector POC ready" APP_LOG_NL);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      connection_count++;
      // Spin up a reflector instance for every inbound connection. If the
      // peer is the AoA locator (CTE only) the CS handshake will simply
      // never start and the reflector instance stays idle until disconnect.
      {
        uint8_t conn = evt->data.evt_connection_opened.connection;
        sl_status_t rc = cs_reflector_create(conn, &cs_reflector_config);
        if (rc != SL_STATUS_OK) {
          app_log_warning("[APP] cs_reflector_create(%u) -> 0x%04lx" APP_LOG_NL,
                          conn, (unsigned long)rc);
        }
      }
      // Do NOT re-advertise while a peer is connected. The CS initiator's
      // scanner is still running, so any new advertising packet would cause
      // it to open a duplicate connection (CS_INITIATOR_MAX_CONNECTIONS=1
      // → 0x1C / SL_STATUS_NO_MORE_RESOURCE → reconnect storm that starves
      // the primary CS procedure). Single-peer is fine for this POC.
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      {
        uint8_t conn = evt->data.evt_connection_closed.connection;
        if (cs_reflector_identify(conn)) {
          (void)cs_reflector_delete(conn);
        }
      }
      connection_count--;
      // Always resume advertising once we're free.
      if (connection_count == 0) {
        sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                           sl_bt_legacy_advertiser_connectable);
        app_assert_status(sc);
      }
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}
