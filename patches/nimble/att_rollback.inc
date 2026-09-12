/* Only for an uncommitted registration transaction holding the host lock.
 * No peer or application callback has observed these handles. */
#if MYNEWT_VAL(BLE_DYNAMIC_SERVICE)
void
ble_att_svr_rollback(uint16_t checkpoint)
{
    if (ble_att_svr_id > checkpoint) {
        ble_att_svr_deregister(checkpoint + 1, ble_att_svr_id);
        ble_att_svr_id = checkpoint;
    }
}
#endif
