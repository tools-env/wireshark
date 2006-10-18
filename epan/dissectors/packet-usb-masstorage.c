/* packet-usb-masstorage.c
 *
 * $Id$
 *
 * usb mass storage dissector
 * Ronnie Sahlberg 2006
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
 

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <epan/packet.h>
#include <epan/emem.h>
#include <epan/conversation.h>
#include <string.h>
#include "packet-usb.h"
#include "packet-scsi.h"

/* protocols and header fields */
static int proto_usb_ms = -1;
static int hf_usb_ms_dCBWSignature = -1;
static int hf_usb_ms_dCBWTag = -1;
static int hf_usb_ms_dCBWDataTransferLength = -1;
static int hf_usb_ms_dCBWFlags = -1;
static int hf_usb_ms_dCBWLUN = -1;
static int hf_usb_ms_dCBWCBLength = -1;

static gint ett_usb_ms = -1;


/* there is one such structure for each masstorage conversation */
typedef struct _usb_ms_conv_info_t {
    emem_tree_t *itl;		/* indexed by LUN */
    emem_tree_t *itlq;		/* pinfo->fd->num */
} usb_ms_conv_info_t;

static void
dissect_usb_ms(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree)
{
    usb_conv_info_t *usb_conv_info;
    usb_ms_conv_info_t *usb_ms_conv_info;
    proto_tree *tree=NULL;
    guint32 signature;
    int offset=0;
    gboolean is_request;
    itl_nexus_t *itl;
    itlq_nexus_t *itlq;

    usb_conv_info=pinfo->usb_conv_info;
    /* verify that we do have a usb_ms_conv_info */
    usb_ms_conv_info=usb_conv_info->masstorage;
    if(!usb_ms_conv_info){
        usb_ms_conv_info=se_alloc(sizeof(usb_ms_conv_info_t));
        usb_ms_conv_info->itl=se_tree_create_non_persistent(EMEM_TREE_TYPE_RED_BLACK, "USB ITL");
        usb_ms_conv_info->itlq=se_tree_create_non_persistent(EMEM_TREE_TYPE_RED_BLACK, "USB ITLQ");
        usb_conv_info->masstorage=usb_ms_conv_info;
    }


    is_request=(pinfo->srcport==NO_ENDPOINT);

    if(check_col(pinfo->cinfo, COL_PROTOCOL))
        col_set_str(pinfo->cinfo, COL_PROTOCOL, "USBMS");

    if(check_col(pinfo->cinfo, COL_INFO))
        col_clear(pinfo->cinfo, COL_INFO);


    if(parent_tree){
        proto_item *ti = NULL;
        ti = proto_tree_add_protocol_format(parent_tree, proto_usb_ms, tvb, 0, -1, "USB Mass Storage");

        tree = proto_item_add_subtree(ti, ett_usb_ms);
    }

    signature=tvb_get_letohl(tvb, offset);
    /* is this a CDB ? */
    if(is_request&&(signature==0x43425355)&&(tvb_length(tvb)==31)){
        tvbuff_t *cdb_tvb;
        int cdbrlen, cdblen;
        guint8 lun, flags;
        guint32 datalen;

        /* dCBWSignature */
        proto_tree_add_item(tree, hf_usb_ms_dCBWSignature, tvb, offset, 4, TRUE);
        offset+=4;

        /* dCBWTag */
        proto_tree_add_item(tree, hf_usb_ms_dCBWTag, tvb, offset, 4, TRUE);
        offset+=4;

        /* dCBWDataTransferLength */
        proto_tree_add_item(tree, hf_usb_ms_dCBWDataTransferLength, tvb, offset, 4, TRUE);
        datalen=tvb_get_letohl(tvb, offset);
        offset+=4;

        /* dCBWFlags */
        proto_tree_add_item(tree, hf_usb_ms_dCBWFlags, tvb, offset, 1, TRUE);
        flags=tvb_get_guint8(tvb, offset);
        offset+=1;

        /* dCBWLUN */
        proto_tree_add_item(tree, hf_usb_ms_dCBWLUN, tvb, offset, 1, TRUE);
        lun=tvb_get_guint8(tvb, offset)&0x0f;
        offset+=1;

        /* make sure we have a ITL structure for this LUN */
        itl=(itl_nexus_t *)se_tree_lookup32(usb_ms_conv_info->itl, lun);
        if(!itl){
            itl=se_alloc(sizeof(itl_nexus_t));
            itl->cmdset=0xff;
            itl->conversation=NULL;
            se_tree_insert32(usb_ms_conv_info->itl, lun, itl);
        }

        /* make sure we have an ITLQ structure for this LUN/transaction */
        itlq=(itlq_nexus_t *)se_tree_lookup32(usb_ms_conv_info->itlq, pinfo->fd->num);
        if(!itlq){
            itlq=se_alloc(sizeof(itlq_nexus_t));
            itlq->lun=lun;
            itlq->scsi_opcode=0xffff;
            itlq->task_flags=0;
            if(datalen){
                if(flags&0x80){
                    itlq->task_flags|=SCSI_DATA_READ;
                } else {
                    itlq->task_flags|=SCSI_DATA_WRITE;
                }
            }
            itlq->data_length=datalen;
            itlq->bidir_data_length=0;
            itlq->fc_time=pinfo->fd->abs_ts;
            itlq->first_exchange_frame=pinfo->fd->num;
            itlq->last_exchange_frame=0;
            itlq->flags=0;
            itlq->alloc_len=0;
            itlq->extra_data=NULL;
            se_tree_insert32(usb_ms_conv_info->itlq, pinfo->fd->num, itlq);
        }

        /* dCBWCBLength */
        proto_tree_add_item(tree, hf_usb_ms_dCBWCBLength, tvb, offset, 1, TRUE);
        cdbrlen=tvb_get_guint8(tvb, offset)&0x1f;
        offset+=1;

        cdblen=cdbrlen;
        if(cdblen>tvb_length_remaining(tvb, offset)){
            cdblen=tvb_length_remaining(tvb, offset);
        }
        if(cdblen){
            cdb_tvb=tvb_new_subset(tvb, offset, cdblen, cdbrlen);
            dissect_scsi_cdb(cdb_tvb, pinfo, parent_tree, SCSI_DEV_UNKNOWN, itlq, itl);
        }
    }
}

void
proto_register_usb_ms(void)
{
    static hf_register_info hf[] = {
        { &hf_usb_ms_dCBWSignature,
        { "Signature", "usbms.dCBWSignature", FT_UINT32, BASE_HEX, 
          NULL, 0x0, "", HFILL }},

        { &hf_usb_ms_dCBWTag,
        { "Tag", "usbms.dCBWTag", FT_UINT32, BASE_HEX, 
          NULL, 0x0, "", HFILL }},

        { &hf_usb_ms_dCBWDataTransferLength,
        { "DataTransferLength", "usbms.dCBWDataTransferLength", FT_UINT32, BASE_DEC, 
          NULL, 0x0, "", HFILL }},

        { &hf_usb_ms_dCBWFlags,
        { "Flags", "usbms.dCBWFlags", FT_UINT8, BASE_HEX, 
          NULL, 0x0, "", HFILL }},

        { &hf_usb_ms_dCBWLUN,
        { "LUN", "usbms.dCBWLUN", FT_UINT8, BASE_HEX, 
          NULL, 0x0f, "", HFILL }},

        { &hf_usb_ms_dCBWCBLength,
        { "CDB Length", "usbms.dCBWCBLength", FT_UINT8, BASE_HEX, 
          NULL, 0x1f, "", HFILL }},

    };
    
    static gint *usb_ms_subtrees[] = {
            &ett_usb_ms,
    };

     
    proto_usb_ms = proto_register_protocol("USB Mass Storage", "USBMS", "usbms");
    proto_register_field_array(proto_usb_ms, hf, array_length(hf));
    proto_register_subtree_array(usb_ms_subtrees, array_length(usb_ms_subtrees));

    register_dissector("usbms", dissect_usb_ms, proto_usb_ms);
}

void
proto_reg_handoff_usb_ms(void)
{
    dissector_handle_t usb_ms_handle;
    usb_ms_handle = create_dissector_handle(dissect_usb_ms, proto_usb_ms);

    dissector_add("usb.bulk", IF_CLASS_MASSTORAGE, usb_ms_handle);
}
