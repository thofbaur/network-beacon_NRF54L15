/*
 * C5 - param_storage.c: versioned, CRC-protected parameter records on top of
 * Zephyr settings, plus the unversioned legacy fallback.
 * Record layout: magic(BE32 0x44534150) version(BE16 1) len(BE16)
 * crc32_ieee(payload)(BE32) payload. See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "fake_settings.h"
#include "minitest.h"
#include "param_storage.h"

static void setup(void)
{
	fake_settings_reset();
}

MT_TEST(c5_crc32_matches_ieee_check_value)
{
	MT_ASSERT_EQ(0xCBF43926U, crc32_ieee((const uint8_t *)"123456789", 9));
}

MT_TEST(c5_save_then_load_roundtrip)
{
	uint8_t in[3] = { 7, 1, 99 };
	uint8_t out[3] = { 0 };

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, sizeof(in)));
	MT_ASSERT_EQ(0, param_storage_load("dsa/test", out, sizeof(out)));
	MT_ASSERT_MEM_EQ(in, out, sizeof(in));
}

MT_TEST(c5_record_layout_is_stable)
{
	uint8_t in[3] = { 1, 2, 3 };
	uint8_t raw[32];

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, sizeof(in)));
	MT_ASSERT_EQ(15, fake_settings_get_raw("dsa/test", raw, sizeof(raw)));
	MT_ASSERT_EQ(0x44534150U, sys_get_be32(&raw[0]));
	MT_ASSERT_EQ(1, sys_get_be16(&raw[4]));
	MT_ASSERT_EQ(3, sys_get_be16(&raw[6]));
	MT_ASSERT_EQ(crc32_ieee(in, 3), sys_get_be32(&raw[8]));
	MT_ASSERT_MEM_EQ(in, &raw[12], 3);
}

MT_TEST(c5_missing_key_is_enoent)
{
	uint8_t out[3];

	MT_ASSERT_EQ(-ENOENT, param_storage_load("dsa/none", out, sizeof(out)));
}

MT_TEST(c5_length_mismatch_is_ebadmsg)
{
	uint8_t in[3] = { 1, 2, 3 };
	uint8_t out[4];

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, sizeof(in)));
	MT_ASSERT_EQ(-EBADMSG, param_storage_load("dsa/test", out, 2));
	MT_ASSERT_EQ(-EBADMSG, param_storage_load("dsa/test", out, 4));
}

MT_TEST(c5_corrupted_payload_fails_crc)
{
	uint8_t in[3] = { 1, 2, 3 };
	uint8_t raw[15];
	uint8_t out[3];

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, sizeof(in)));
	fake_settings_get_raw("dsa/test", raw, sizeof(raw));
	raw[13] ^= 0x01;
	fake_settings_put_raw("dsa/test", raw, sizeof(raw));
	MT_ASSERT_EQ(-EBADMSG, param_storage_load("dsa/test", out, sizeof(out)));
}

MT_TEST(c5_wrong_magic_or_version_is_ebadmsg)
{
	uint8_t in[3] = { 1, 2, 3 };
	uint8_t raw[15];
	uint8_t out[3];

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, sizeof(in)));
	fake_settings_get_raw("dsa/test", raw, sizeof(raw));
	raw[0] ^= 0xFF;
	fake_settings_put_raw("dsa/test", raw, sizeof(raw));
	MT_ASSERT_EQ(-EBADMSG, param_storage_load("dsa/test", out, sizeof(out)));
	raw[0] ^= 0xFF;
	raw[5] = 2;
	fake_settings_put_raw("dsa/test", raw, sizeof(raw));
	MT_ASSERT_EQ(-EBADMSG, param_storage_load("dsa/test", out, sizeof(out)));
}

MT_TEST(c5_invalid_arguments_are_rejected)
{
	uint8_t buf[65] = { 0 };

	MT_ASSERT_EQ(-EINVAL, param_storage_load(NULL, buf, 1));
	MT_ASSERT_EQ(-EINVAL, param_storage_load("dsa/test", NULL, 1));
	MT_ASSERT_EQ(-EINVAL, param_storage_load("dsa/test", buf, 0));
	MT_ASSERT_EQ(-EINVAL, param_storage_load("dsa/test", buf, 65));
	MT_ASSERT_EQ(-EINVAL, param_storage_save("dsa/test", buf, 0));
	MT_ASSERT_EQ(-EINVAL, param_storage_save("dsa/test", buf, 65));
	MT_ASSERT_EQ(0, param_storage_save("dsa/test", buf, 64));
}

MT_TEST(c5_settings_errors_are_propagated)
{
	uint8_t buf[3] = { 0 };

	fake_settings_set_save_error(-EIO);
	MT_ASSERT_EQ(-EIO, param_storage_save("dsa/test", buf, 3));
	fake_settings_set_save_error(0);
	fake_settings_set_load_error(-EIO);
	MT_ASSERT_EQ(-EIO, param_storage_load("dsa/test", buf, 3));
}

MT_TEST(c5_settings_init_error_is_propagated)
{
	uint8_t buf[3] = { 0 };

	fake_settings_set_init_error(-ENODEV);
	MT_ASSERT_EQ(-ENODEV, param_storage_save("dsa/test", buf, 3));
}

MT_TEST(c5_legacy_load_reads_exact_raw_value)
{
	uint8_t raw = 42;
	uint8_t out = 0;

	fake_settings_put_raw("dsa/legacy", &raw, 1);
	MT_ASSERT_EQ(0, param_storage_load_legacy("dsa/legacy", &out, 1));
	MT_ASSERT_EQ(42, out);
}

MT_TEST(c5_legacy_load_rejects_other_lengths)
{
	uint8_t raw[2] = { 1, 2 };
	uint8_t out = 0;

	fake_settings_put_raw("dsa/legacy", raw, 2);
	MT_ASSERT_EQ(-EINVAL, param_storage_load_legacy("dsa/legacy", &out, 1));
	MT_ASSERT_EQ(-ENOENT, param_storage_load_legacy("dsa/none", &out, 1));
}

MT_TEST(c5_delete_removes_record)
{
	uint8_t in[3] = { 1, 2, 3 };
	uint8_t out[3];

	MT_ASSERT_EQ(0, param_storage_save("dsa/test", in, 3));
	MT_ASSERT_EQ(0, param_storage_delete("dsa/test"));
	MT_ASSERT_EQ(-ENOENT, param_storage_load("dsa/test", out, 3));
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c5_settings_init_error_is_propagated),
	MT_CASE(c5_crc32_matches_ieee_check_value),
	MT_CASE(c5_save_then_load_roundtrip),
	MT_CASE(c5_record_layout_is_stable),
	MT_CASE(c5_missing_key_is_enoent),
	MT_CASE(c5_length_mismatch_is_ebadmsg),
	MT_CASE(c5_corrupted_payload_fails_crc),
	MT_CASE(c5_wrong_magic_or_version_is_ebadmsg),
	MT_CASE(c5_invalid_arguments_are_rejected),
	MT_CASE(c5_settings_errors_are_propagated),
	MT_CASE(c5_legacy_load_reads_exact_raw_value),
	MT_CASE(c5_legacy_load_rejects_other_lengths),
	MT_CASE(c5_delete_removes_record))
