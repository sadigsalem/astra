/*
 * Astra Module: MPEG-TS (MPTS Demux)
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2015, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <astra.h>

typedef struct
{
    char type[6];
    uint16_t origin_pid;
    uint16_t custom_pid;
    bool is_set;
} map_item_t;

struct module_data_t
{
    MODULE_STREAM_DATA();

    /* Options */
    struct
    {
        const char *name;
        int pnr;
        int set_pnr;
        int set_tsid;
        bool no_sdt;
        bool no_eit;
        bool no_reload;
        bool cas;

        uint8_t service_type;
        uint8_t *service_provider;
        uint8_t *service_name;

        bool pass_sdt;
        bool pass_eit;
    } config;

    /* */
    asc_list_t *map;
    uint16_t pid_map[MAX_PID];
    uint8_t custom_ts[TS_PACKET_SIZE];

    mpegts_psi_t *pat;
    mpegts_psi_t *cat;
    mpegts_psi_t *pmt;
    mpegts_psi_t *sdt;
    mpegts_psi_t *eit;

    mpegts_packet_type_t stream[MAX_PID];

    uint16_t tsid;
    mpegts_psi_t *custom_pat;
    mpegts_psi_t *custom_cat;
    mpegts_psi_t *custom_pmt;
    mpegts_psi_t *custom_sdt;

    /* */
    uint8_t sdt_original_section_id;
    uint8_t sdt_max_section_id;
    uint32_t *sdt_checksum_list;

    uint8_t eit_cc;

    uint8_t pat_version;
    asc_timer_t *si_timer;
};

#define MSG(_msg) "[channel %s] " _msg, mod->config.name

static void stream_reload(module_data_t *mod)
{
    memset(mod->stream, 0, sizeof(mod->stream));

    for(int __i = 0; __i < MAX_PID; ++__i)
    {
        if(mod->__stream.pid_list[__i])
            module_stream_demux_leave_pid(mod, __i);
    }

    mod->pat->crc32 = 0;
    mod->pmt->crc32 = 0;

    mod->stream[0x00] = MPEGTS_PACKET_PAT;
    module_stream_demux_join_pid(mod, 0x00);

    if(mod->config.cas)
    {
        mod->cat->crc32 = 0;
        mod->stream[0x01] = MPEGTS_PACKET_CAT;
        module_stream_demux_join_pid(mod, 0x01);
    }

    if(mod->config.no_sdt == false)
    {
        mod->stream[0x11] = MPEGTS_PACKET_SDT;
        module_stream_demux_join_pid(mod, 0x11);
        if(mod->sdt_checksum_list)
        {
            free(mod->sdt_checksum_list);
            mod->sdt_checksum_list = NULL;
        }
    }

    if(mod->config.no_eit == false)
    {
        mod->stream[0x12] = MPEGTS_PACKET_EIT;
        module_stream_demux_join_pid(mod, 0x12);

        mod->stream[0x14] = MPEGTS_PACKET_TDT;
        module_stream_demux_join_pid(mod, 0x14);
    }

    if(mod->map)
    {
        asc_list_for(mod->map)
        {
            map_item_t *map_item = (map_item_t *)asc_list_data(mod->map);
            map_item->is_set = false;
        }
    }
}

static void on_si_timer(void *arg)
{
    module_data_t *mod = (module_data_t *)arg;

    if(mod->custom_pat)
        mpegts_psi_demux(mod->custom_pat, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->custom_cat)
        mpegts_psi_demux(mod->custom_cat, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->custom_pmt)
        mpegts_psi_demux(mod->custom_pmt, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->custom_sdt)
        mpegts_psi_demux(mod->custom_sdt, (ts_callback_t)__module_stream_send, &mod->__stream);
}

static uint16_t map_custom_pid(module_data_t *mod, uint16_t pid, const char *type)
{
    asc_list_for(mod->map)
    {
        map_item_t *map_item = (map_item_t *)asc_list_data(mod->map);
        if(map_item->is_set)
            continue;

        if(   (map_item->origin_pid && map_item->origin_pid == pid)
           || (!strcmp(map_item->type, type)) )
        {
            map_item->is_set = true;
            mod->pid_map[pid] = map_item->custom_pid;

            return map_item->custom_pid;
        }
    }
    return 0;
}

/*
 *  ____   _  _____
 * |  _ \ / \|_   _|
 * | |_) / _ \ | |
 * |  __/ ___ \| |
 * |_| /_/   \_\_|
 *
 */

static void on_pat(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x00)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        mpegts_psi_demux(mod->custom_pat, (ts_callback_t)__module_stream_send, &mod->__stream);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PAT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PAT changed. Reload stream info"));
        stream_reload(mod);
    }

    psi->crc32 = crc32;

    mod->tsid = PAT_GET_TSID(psi);

    const uint8_t *pointer;

    PAT_ITEMS_FOREACH(psi, pointer)
    {
        const uint16_t pnr = PAT_ITEM_GET_PNR(psi, pointer);
        if(!pnr)
            continue;

        if(!mod->config.pnr)
            mod->config.pnr = pnr;

        if(pnr == mod->config.pnr)
        {
            const uint16_t pid = PAT_ITEM_GET_PID(psi, pointer);
            mod->stream[pid] = MPEGTS_PACKET_PMT;
            module_stream_demux_join_pid(mod, pid);
            mod->pmt->pid = pid;
            mod->pmt->crc32 = 0;
            break;
        }
    }

    if(PAT_ITEMS_EOL(psi, pointer))
    {
        mod->custom_pat->buffer_size = 0;
        asc_log_error(MSG("PAT: stream with id %d is not found"), mod->config.pnr);
        return;
    }

    const uint8_t pat_version = PAT_GET_VERSION(mod->custom_pat) + 1;
    const uint16_t tsid = (mod->config.set_tsid) ? mod->config.set_tsid : mod->tsid;
    PAT_INIT(mod->custom_pat, tsid, pat_version);
    memcpy(PAT_ITEMS_FIRST(mod->custom_pat), pointer, 4);

    mod->custom_pmt->pid = mod->pmt->pid;

    if(mod->config.set_pnr)
    {
        uint8_t *custom_pointer = PAT_ITEMS_FIRST(mod->custom_pat);
        PAT_ITEM_SET_PNR(mod->custom_pat, custom_pointer, mod->config.set_pnr);
    }

    if(mod->map)
    {
        uint16_t custom_pid = map_custom_pid(mod, mod->pmt->pid, "pmt");
        if(custom_pid != 0)
        {
            uint8_t *custom_pointer = PAT_ITEMS_FIRST(mod->custom_pat);
            PAT_ITEM_SET_PID(mod->custom_pat, custom_pointer, custom_pid);
            mod->custom_pmt->pid = custom_pid;
        }
    }

    mod->pat_version = (mod->pat_version + 1) & 0x0F;
    PAT_SET_VERSION(mod->custom_pat, mod->pat_version);

    mod->custom_pat->buffer_size = 8 + 4 + CRC32_SIZE;
    PSI_SET_SIZE(mod->custom_pat);
    PSI_SET_CRC32(mod->custom_pat);

    mpegts_psi_demux(mod->custom_pat, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = MPEGTS_PACKET_UNKNOWN;
}

/*
 *   ____    _  _____
 *  / ___|  / \|_   _|
 * | |     / _ \ | |
 * | |___ / ___ \| |
 *  \____/_/   \_\_|
 *
 */

static void on_cat(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x01)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        mpegts_psi_demux(mod->custom_cat, (ts_callback_t)__module_stream_send, &mod->__stream);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("CAT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("CAT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    psi->crc32 = crc32;

    const uint8_t *desc_pointer;

    CAT_DESC_FOREACH(psi, desc_pointer)
    {
        if(desc_pointer[0] == 0x09)
        {
            const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
            if(mod->stream[ca_pid] == MPEGTS_PACKET_UNKNOWN && ca_pid != NULL_TS_PID)
            {
                mod->stream[ca_pid] = MPEGTS_PACKET_CA;
                if(mod->pid_map[ca_pid] == MAX_PID)
                    mod->pid_map[ca_pid] = 0;
                module_stream_demux_join_pid(mod, ca_pid);
            }
        }
    }

    memcpy(mod->custom_cat->buffer, psi->buffer, psi->buffer_size);
    mod->custom_cat->buffer_size = psi->buffer_size;
    mod->custom_cat->cc = 0;

    mpegts_psi_demux(mod->custom_cat, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = MPEGTS_PACKET_UNKNOWN;
}

/*
 *  ____  __  __ _____
 * |  _ \|  \/  |_   _|
 * | |_) | |\/| | | |
 * |  __/| |  | | | |
 * |_|   |_|  |_| |_|
 *
 */

static void on_pmt(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x02)
        return;

    // check pnr
    const uint16_t pnr = PMT_GET_PNR(psi);
    if(pnr != mod->config.pnr)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        mpegts_psi_demux(mod->custom_pmt, (ts_callback_t)__module_stream_send, &mod->__stream);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PMT checksum error"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PMT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    psi->crc32 = crc32;

    uint16_t skip = 12;
    memcpy(mod->custom_pmt->buffer, psi->buffer, 10);

    const uint16_t pcr_pid = PMT_GET_PCR(psi);
    bool join_pcr = true;

    const uint8_t *desc_pointer;

    PMT_DESC_FOREACH(psi, desc_pointer)
    {
        if(desc_pointer[0] == 0x09)
        {
            if(!mod->config.cas)
                continue;

            const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
            if(mod->stream[ca_pid] == MPEGTS_PACKET_UNKNOWN && ca_pid != NULL_TS_PID)
            {
                mod->stream[ca_pid] = MPEGTS_PACKET_CA;
                if(mod->pid_map[ca_pid] == MAX_PID)
                    mod->pid_map[ca_pid] = 0;
                module_stream_demux_join_pid(mod, ca_pid);
            }
        }

        const uint8_t size = desc_pointer[1] + 2;
        memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
        skip += size;
    }

    {
        const uint16_t size = skip - 12; // 12 - PMT header
        mod->custom_pmt->buffer[10] = (psi->buffer[10] & 0xF0) | ((size >> 8) & 0x0F);
        mod->custom_pmt->buffer[11] = (size & 0xFF);
    }

    if(mod->config.set_pnr)
    {
        PMT_SET_PNR(mod->custom_pmt, mod->config.set_pnr);
    }

    const uint8_t *pointer;
    PMT_ITEMS_FOREACH(psi, pointer)
    {
        const uint16_t pid = PMT_ITEM_GET_PID(psi, pointer);

        if(mod->pid_map[pid] == MAX_PID) // skip filtered pid
            continue;

        const uint8_t item_type = PMT_ITEM_GET_TYPE(psi, pointer);
        mpegts_packet_type_t mpegts_type = mpegts_pes_type(item_type);
        const uint8_t *language_desc = NULL;

        const uint16_t skip_last = skip;

        memcpy(&mod->custom_pmt->buffer[skip], pointer, 5);
        skip += 5;

        mod->stream[pid] = MPEGTS_PACKET_PES;
        module_stream_demux_join_pid(mod, pid);

        if(pid == pcr_pid)
            join_pcr = false;

        PMT_ITEM_DESC_FOREACH(pointer, desc_pointer)
        {
            const uint8_t desc_type = desc_pointer[0];

            if(desc_type == 0x09)
            {
                if(!mod->config.cas)
                    continue;

                const uint16_t ca_pid = DESC_CA_PID(desc_pointer);
                if(mod->stream[ca_pid] == MPEGTS_PACKET_UNKNOWN && ca_pid != NULL_TS_PID)
                {
                    mod->stream[ca_pid] = MPEGTS_PACKET_CA;
                    if(mod->pid_map[ca_pid] == MAX_PID)
                        mod->pid_map[ca_pid] = 0;
                    module_stream_demux_join_pid(mod, ca_pid);
                }
            }
            else if(desc_type == 0x0A)
            {
                language_desc = desc_pointer;
            }
            else if(item_type == 0x06)
            {
                switch(desc_type)
                {
                    case 0x59:
                        mpegts_type = MPEGTS_PACKET_SUB;
                        break;
                    case 0x6A:
                    case 0x7A:
                        mpegts_type = MPEGTS_PACKET_AUDIO;
                        break;
                    default:
                        break;
                }
            }

            const uint8_t size = desc_pointer[1] + 2;
            memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
            skip += size;
        }

        {
            const uint16_t size = skip - skip_last - 5;
            mod->custom_pmt->buffer[skip_last + 3] = (size << 8) & 0x0F;
            mod->custom_pmt->buffer[skip_last + 4] = (size & 0xFF);
        }

        if(mod->map)
        {
            uint16_t custom_pid = 0;

            switch(mpegts_type)
            {
                case MPEGTS_PACKET_VIDEO:
                {
                    custom_pid = map_custom_pid(mod, pid, "video");
                    break;
                }
                case MPEGTS_PACKET_AUDIO:
                {
                    if(language_desc)
                    {
                        char lang[4];
                        lang[0] = language_desc[2];
                        lang[1] = language_desc[3];
                        lang[2] = language_desc[4];
                        lang[3] = 0;
                        custom_pid = map_custom_pid(mod, pid, lang);
                    }
                    if(!custom_pid)
                        custom_pid = map_custom_pid(mod, pid, "audio");
                    break;
                }
                default:
                {
                    custom_pid = map_custom_pid(mod, pid, "");
                    break;
                }
            }

            if(custom_pid != 0)
            {
                PMT_ITEM_SET_PID(mod->custom_pmt, &mod->custom_pmt->buffer[skip_last], custom_pid);
            }
        }
    }
    mod->custom_pmt->buffer_size = skip + CRC32_SIZE;

    if(join_pcr)
    {
        mod->stream[pcr_pid] = MPEGTS_PACKET_PES;
        if(mod->pid_map[pcr_pid] == MAX_PID)
            mod->pid_map[pcr_pid] = 0;
        module_stream_demux_join_pid(mod, pcr_pid);
    }

    if(mod->map)
    {
        if(mod->pid_map[pcr_pid])
            PMT_SET_PCR(mod->custom_pmt, mod->pid_map[pcr_pid]);
    }

    PSI_SET_SIZE(mod->custom_pmt);
    PSI_SET_CRC32(mod->custom_pmt);
    mpegts_psi_demux(mod->custom_pmt, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = MPEGTS_PACKET_UNKNOWN;
}

/*
 *  ____  ____ _____
 * / ___||  _ \_   _|
 * \___ \| | | || |
 *  ___) | |_| || |
 * |____/|____/ |_|
 *
 */

static void on_sdt(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    if(psi->buffer[0] != 0x42)
        return;

    if((psi->buffer[1] & 0x80) != 0x80)
        return;

    if((psi->buffer[5] & 0x01) != 0x01)
        return;

    const uint32_t crc32 = PSI_GET_CRC32(psi);

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("SDT checksum error"));
        return;
    }

    // check changes
    if(!mod->sdt_checksum_list)
    {
        const uint8_t max_section_id = SDT_GET_LSECTION_NUMBER(psi);
        mod->sdt_max_section_id = max_section_id;
        mod->sdt_checksum_list = (uint32_t *)calloc(max_section_id + 1, sizeof(uint32_t));
    }
    const uint8_t section_id = SDT_GET_CSECTION_NUMBER(psi);
    if(section_id > mod->sdt_max_section_id)
    {
        asc_log_warning(MSG("SDT: section_number is greater then section_last_number"));
        return;
    }
    if(mod->sdt_checksum_list[section_id] == crc32)
    {
        if(mod->sdt_original_section_id == section_id)
            mpegts_psi_demux(mod->custom_sdt, (ts_callback_t)__module_stream_send, &mod->__stream);

        return;
    }

    if(mod->sdt_checksum_list[section_id] != 0)
    {
        asc_log_warning(MSG("SDT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    mod->sdt_checksum_list[section_id] = crc32;

    const uint8_t *pointer;
    SDT_ITEMS_FOREACH(psi, pointer)
    {
        if(SDT_ITEM_GET_SID(psi, pointer) == mod->config.pnr)
            break;
    }

    if(SDT_ITEMS_EOL(psi, pointer))
        return;

    mod->sdt_original_section_id = section_id;

    memcpy(mod->custom_sdt->buffer, psi->buffer, 11); // copy SDT header
    SDT_SET_CSECTION_NUMBER(mod->custom_sdt, 0);
    SDT_SET_LSECTION_NUMBER(mod->custom_sdt, 0);
    SDT_SET_TSID(mod->custom_sdt, (mod->config.set_tsid != 0) ? mod->config.set_tsid : mod->tsid);

    uint8_t *custom_pointer = &mod->custom_sdt->buffer[11];
    uint16_t custom_pointer_skip = 5;

    memcpy(custom_pointer, pointer, 5); // copy SDT item header
    if(mod->config.set_pnr)
        SDT_ITEM_SET_SID(mod->custom_sdt, custom_pointer, mod->config.set_pnr);

    const uint8_t *service_provider = NULL;
    const uint8_t *service_name = NULL;
    uint8_t service_type = 0x00;

    const uint8_t *desc_pointer;
    SDT_ITEM_DESC_FOREACH(pointer, desc_pointer)
    {
        const uint8_t desc_length = desc_pointer[1] + 2;
        if(desc_pointer[0] == 0x48)
        {
            service_type = desc_pointer[2];
            service_provider = &desc_pointer[3];
            service_name = &desc_pointer[3 + 1 + service_provider[0]];
        }
        else
        {
            memcpy(&custom_pointer[custom_pointer_skip], desc_pointer, desc_length);
            custom_pointer_skip += desc_length;
        }
    }

    if(mod->config.service_provider)
        service_provider = mod->config.service_provider;
    if(mod->config.service_name)
        service_name = mod->config.service_name;
    if(mod->config.service_type != 0x00)
        service_type = mod->config.service_type;

    if(service_provider || service_name)
    {
        static const uint8_t empty_service_value[1] = { 0x00 };
        if(!service_provider)
            service_provider = empty_service_value;
        if(!service_name)
            service_name = empty_service_value;

        const uint8_t service_provider_length = 1 + service_provider[0];
        const uint8_t service_name_length = 1 + service_name[0];

        custom_pointer[custom_pointer_skip + 0] = 0x48;
        custom_pointer[custom_pointer_skip + 1] =
            1 + /* service_type */
            service_provider_length +
            service_name_length;
        custom_pointer[custom_pointer_skip + 2] = service_type;
        custom_pointer_skip += 3;
        memcpy(&custom_pointer[custom_pointer_skip], service_provider, service_provider_length);
        custom_pointer_skip += service_provider_length;
        memcpy(&custom_pointer[custom_pointer_skip], service_name, service_name_length);
        custom_pointer_skip += service_name_length;
    }

    const uint16_t item_length = custom_pointer_skip - 5;
    custom_pointer[3] = (custom_pointer[3] & 0xF0) | ((item_length >> 8) & 0x0F);
    custom_pointer[4] = (item_length & 0xFF);

    mod->custom_sdt->buffer_size = 11 + 5 + item_length + CRC32_SIZE;

    PSI_SET_SIZE(mod->custom_sdt);
    PSI_SET_CRC32(mod->custom_sdt);

    mpegts_psi_demux(mod->custom_sdt, (ts_callback_t)__module_stream_send, &mod->__stream);

    if(mod->config.no_reload)
        mod->stream[psi->pid] = MPEGTS_PACKET_UNKNOWN;
}

/*
 *  _____ ___ _____
 * | ____|_ _|_   _|
 * |  _|  | |  | |
 * | |___ | |  | |
 * |_____|___| |_|
 *
 */

static void on_eit(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = (module_data_t *)arg;

    const uint8_t table_id = psi->buffer[0];
    const bool is_actual_eit = (table_id == 0x4E || (table_id >= 0x50 && table_id <= 0x5F));
    if(!is_actual_eit)
        return;

    if(mod->tsid != EIT_GET_TSID(psi))
        return;

    if(mod->config.pnr != EIT_GET_PNR(psi))
        return;

    psi->cc = mod->eit_cc;

    if(mod->config.set_pnr)
        EIT_SET_PNR(psi, mod->config.set_pnr);
    if(mod->config.set_tsid)
        EIT_SET_TSID(psi, mod->config.set_tsid);

    PSI_SET_CRC32(psi);

    mpegts_psi_demux(psi, (ts_callback_t)__module_stream_send, &mod->__stream);

    mod->eit_cc = psi->cc;
}

/*
 *  _____ ____
 * |_   _/ ___|
 *   | | \___ \
 *   | |  ___) |
 *   |_| |____/
 *
 */

static void on_ts(module_data_t *mod, const uint8_t *ts)
{
    const uint16_t pid = TS_GET_PID(ts);
    if(!module_stream_demux_check_pid(mod, pid))
        return;

    if(pid == NULL_TS_PID)
        return;

    switch(mod->stream[pid])
    {
        case MPEGTS_PACKET_PES:
            break;
        case MPEGTS_PACKET_PAT:
            mpegts_psi_mux(mod->pat, ts, on_pat, mod);
            return;
        case MPEGTS_PACKET_CAT:
            mpegts_psi_mux(mod->cat, ts, on_cat, mod);
            return;
        case MPEGTS_PACKET_PMT:
            mpegts_psi_mux(mod->pmt, ts, on_pmt, mod);
            return;
        case MPEGTS_PACKET_SDT:
            if(mod->config.pass_sdt)
                break;
            mpegts_psi_mux(mod->sdt, ts, on_sdt, mod);
            return;
        case MPEGTS_PACKET_EIT:
            if(mod->config.pass_eit)
                break;
            mpegts_psi_mux(mod->eit, ts, on_eit, mod);
            return;
        case MPEGTS_PACKET_UNKNOWN:
            return;
        default:
            break;
    }

    if(mod->pid_map[pid] == MAX_PID)
        return;

    if(mod->map)
    {
        const uint16_t custom_pid = mod->pid_map[pid];
        if(custom_pid)
        {
            memcpy(mod->custom_ts, ts, TS_PACKET_SIZE);
            TS_SET_PID(mod->custom_ts, custom_pid);
            module_stream_send(mod, mod->custom_ts);
            return;
        }
    }

    module_stream_send(mod, ts);
}

/*
 *  __  __           _       _
 * |  \/  | ___   __| |_   _| | ___
 * | |\/| |/ _ \ / _` | | | | |/ _ \
 * | |  | | (_) | (_| | |_| | |  __/
 * |_|  |_|\___/ \__,_|\__,_|_|\___|
 *
 */

static void module_init(module_data_t *mod)
{
    module_stream_init(mod, on_ts);
    module_stream_demux_set(mod, NULL, NULL);

    module_option_string("name", &mod->config.name, NULL);
    asc_assert(mod->config.name != NULL, "[channel] option 'name' is required");

    module_option_number("set_pnr", &mod->config.set_pnr);
    module_option_number("set_tsid", &mod->config.set_tsid);

    module_option_boolean("cas", &mod->config.cas);

    mod->pat = mpegts_psi_init(MPEGTS_PACKET_PAT, 0);
    mod->pmt = mpegts_psi_init(MPEGTS_PACKET_PMT, MAX_PID);
    mod->custom_pat = mpegts_psi_init(MPEGTS_PACKET_PAT, 0);
    mod->custom_pmt = mpegts_psi_init(MPEGTS_PACKET_PMT, MAX_PID);
    mod->stream[0] = MPEGTS_PACKET_PAT;
    module_stream_demux_join_pid(mod, 0);
    if(mod->config.cas)
    {
        mod->cat = mpegts_psi_init(MPEGTS_PACKET_CAT, 1);
        mod->custom_cat = mpegts_psi_init(MPEGTS_PACKET_CAT, 1);
        mod->stream[1] = MPEGTS_PACKET_CAT;
        module_stream_demux_join_pid(mod, 1);
    }

    module_option_boolean("no_sdt", &mod->config.no_sdt);
    if(mod->config.no_sdt == false)
    {
        mod->sdt = mpegts_psi_init(MPEGTS_PACKET_SDT, 0x11);
        mod->custom_sdt = mpegts_psi_init(MPEGTS_PACKET_SDT, 0x11);
        mod->stream[0x11] = MPEGTS_PACKET_SDT;
        module_stream_demux_join_pid(mod, 0x11);

        module_option_boolean("pass_sdt", &mod->config.pass_sdt);
    }

    module_option_boolean("no_eit", &mod->config.no_eit);
    if(mod->config.no_eit == false)
    {
        mod->eit = mpegts_psi_init(MPEGTS_PACKET_EIT, 0x12);
        mod->stream[0x12] = MPEGTS_PACKET_EIT;
        module_stream_demux_join_pid(mod, 0x12);

        mod->stream[0x14] = MPEGTS_PACKET_TDT;
        module_stream_demux_join_pid(mod, 0x14);

        module_option_boolean("pass_eit", &mod->config.pass_eit);
    }

    module_option_boolean("no_reload", &mod->config.no_reload);
    if(mod->config.no_reload)
        mod->si_timer = asc_timer_init(500, on_si_timer, mod);

    lua_getfield(lua, MODULE_OPTIONS_IDX, "service_provider");
    if(lua_isstring(lua, -1))
    {
        const uint8_t service_provider_length = luaL_len(lua, -1);
        mod->config.service_provider = (uint8_t *)malloc(1 + service_provider_length);
        mod->config.service_provider[0] = service_provider_length;
        if(service_provider_length > 0)
        {
            const char *service_provider = lua_tostring(lua, -1);
            memcpy(&mod->config.service_provider[1], service_provider, service_provider_length);
        }
    }
    lua_pop(lua, 1); // service_provider

    lua_getfield(lua, MODULE_OPTIONS_IDX, "service_name");
    if(lua_isstring(lua, -1))
    {
        const uint8_t service_name_length = luaL_len(lua, -1);
        mod->config.service_name = (uint8_t *)malloc(1 + service_name_length);
        mod->config.service_name[0] = service_name_length;
        if(service_name_length > 0)
        {
            const char *service_name = lua_tostring(lua, -1);
            memcpy(&mod->config.service_name[1], service_name, service_name_length);
        }
    }
    lua_pop(lua, 1); // service_name

    lua_getfield(lua, -1, "service_type");
    if(lua_isnumber(lua, -1))
    {
        mod->config.service_type = (uint8_t)lua_tointeger(lua, -1);
    }
    lua_pop(lua, 1); // service_type

    lua_getfield(lua, MODULE_OPTIONS_IDX, "map");
    if(lua_istable(lua, -1))
    {
        mod->map = asc_list_init();
        lua_foreach(lua, -2)
        {
            asc_assert((lua_type(lua, -1) == LUA_TTABLE), "option 'map': wrong type");
            asc_assert((luaL_len(lua, -1) == 2), "option 'map': wrong format");

            lua_rawgeti(lua, -1, 1);
            const char *key = lua_tostring(lua, -1);
            asc_assert((luaL_len(lua, -1) <= 5), "option 'map': key is too large");
            lua_pop(lua, 1);

            lua_rawgeti(lua, -1, 2);
            int val = lua_tonumber(lua, -1);
            asc_assert((val > 0 && val < NULL_TS_PID), "option 'map': value is out of range");
            lua_pop(lua, 1);

            map_item_t *map_item = (map_item_t *)calloc(1, sizeof(map_item_t));
            strcpy(map_item->type, key);

            if(key[0] >= '1' && key[0] <= '9')
                map_item->origin_pid = atoi(key);
            map_item->custom_pid = val;
            asc_list_insert_tail(mod->map, map_item);
        }
    }
    lua_pop(lua, 1); // map

    lua_getfield(lua, MODULE_OPTIONS_IDX, "filter");
    if(lua_istable(lua, -1))
    {
        lua_foreach(lua, -2)
        {
            const int pid = lua_tonumber(lua, -1);
            mod->pid_map[pid] = MAX_PID;
        }
    }
    lua_pop(lua, 1); // filter

    lua_getfield(lua, MODULE_OPTIONS_IDX, "filter~");
    if(lua_istable(lua, -1))
    {
        for(uint32_t i = 0; i < ASC_ARRAY_SIZE(mod->pid_map); ++i)
            mod->pid_map[i] = MAX_PID;

        lua_foreach(lua, -2)
        {
            const int pid = lua_tonumber(lua, -1);
            mod->pid_map[pid] = 0;
        }
    }
    lua_pop(lua, 1); // filter~
}

static void module_destroy(module_data_t *mod)
{
    module_stream_destroy(mod);

    mpegts_psi_destroy(mod->pat);
    if(mod->cat)
    {
        mpegts_psi_destroy(mod->cat);
        mpegts_psi_destroy(mod->custom_cat);
    }
    mpegts_psi_destroy(mod->pmt);
    mpegts_psi_destroy(mod->custom_pat);
    mpegts_psi_destroy(mod->custom_pmt);

    if(mod->sdt)
    {
        mpegts_psi_destroy(mod->sdt);
        mpegts_psi_destroy(mod->custom_sdt);

        if(mod->sdt_checksum_list)
            free(mod->sdt_checksum_list);
    }

    if(mod->eit)
        mpegts_psi_destroy(mod->eit);

    if(mod->map)
    {
        asc_list_clear(mod->map)
        {
            free(asc_list_data(mod->map));
        }
        asc_list_destroy(mod->map);
    }

    if(mod->si_timer)
        asc_timer_destroy(mod->si_timer);
}

static const char * module_name(void)
{
    return "mpegts/channel";
}

MODULE_STREAM_METHODS()
MODULE_LUA_METHODS()
{
    MODULE_STREAM_METHODS_REF()
};
MODULE_LUA_REGISTER(channel)
