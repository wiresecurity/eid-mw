#include "cache.h"
#include "backend.h"

#include "xmlmap.h"
#include "xsdloc.h"

#include <string.h>
#include "p11.h"

#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlreader.h>

// libxml2 has a function to write Base64-encoded data, but no function to read
// the same data, so we need our own decoder...
#include <b64/base64dec.h>

#include <assert.h>

#define check_xml(call) if((rc = call) < 0) { \
	be_log(EID_VWR_LOG_DETAIL, "Error while dealing with file (calling '%s'): %d", #call, rc); \
	goto out; \
}

static int write_attributes(xmlTextWriterPtr writer, struct attribute_desc *attribute) {
	int rc = 0;
	char* val = NULL;
	while(attribute->name) {
		int have_cache = cache_have_label(attribute->label);
		if(attribute->reqd && !have_cache) {
			be_log(EID_VWR_LOG_ERROR, "Could not write file: no data found for required label %s", attribute->label);
			return -1;
		}
		if(have_cache) {
			val = cache_get_xmlform(attribute->label);
			if(strlen(val) || attribute->reqd) {
				check_xml(xmlTextWriterWriteAttribute(writer, BAD_CAST attribute->name, BAD_CAST val));
			}
			free(val);
			val = NULL;
		}
		attribute++;
	}

	rc = 0;
out:
	if(val != NULL) {
		free(val);
	}
	return rc;
}

static int write_elements(xmlTextWriterPtr writer, struct element_desc *element) {
	int rc;
	char* val = NULL;
	while(element->name) {
		if(element->label == NULL) {
			assert(element->child_elements != NULL);
			check_xml(xmlTextWriterStartElement(writer, BAD_CAST element->name));
			if(element->attributes != NULL) {
				write_attributes(writer, element->attributes);
			}
			check_xml(write_elements(writer, element->child_elements));
			check_xml(xmlTextWriterEndElement(writer));
		} else {
			int have_cache = cache_have_label(element->label);
			assert(element->child_elements == NULL);

			if(element->reqd && !have_cache) {
				be_log(EID_VWR_LOG_ERROR, "Could not write file: no data found for required label %s", element->label);
				return -1;
			}
			if(have_cache) {
				val = cache_get_xmlform(element->label);
				if(!element->is_b64) {
					check_xml(xmlTextWriterWriteElement(writer, BAD_CAST element->name, BAD_CAST cache_get_xmlform(element->label)));
				} else {
					const struct eid_vwr_cache_item *item = cache_get_data(element->label);
					check_xml(xmlTextWriterStartElement(writer, BAD_CAST element->name));
					check_xml(xmlTextWriterWriteBase64(writer, item->data, 0, item->len));
					check_xml(xmlTextWriterEndElement(writer));
				}
				free(val);
				val = NULL;
			}
		}
		element++;
	}
	rc=0;
out:
	if(val != NULL) {
		free(val);
	}
	return rc;
}

/*
 * TODO: make this execute automatically when we enter STATE_TOKEN_WAIT, so
 * that later on we can just read the data when we need to.
 */
int eid_vwr_gen_xml(void* data) {
	xmlTextWriterPtr writer = NULL;
	int rc;
	xmlBufferPtr buf;

	buf = xmlBufferCreate();
	if(buf == NULL) {
		be_log(EID_VWR_LOG_COARSE, "Could not generate XML format: error creating the xml buffer");
		rc = -1;
		goto out;
	}
	writer = xmlNewTextWriterMemory(buf, 0);
	if(writer == NULL) {
		be_log(EID_VWR_LOG_ERROR, "Could not open file");
		rc = -1;
		goto out;
	}

	check_xml(xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL));
	check_xml(write_elements(writer, toplevel));
	check_xml(xmlTextWriterEndDocument(writer));

	cache_add("xml", buf->content, strlen(buf->content));

	rc=0;
out:
	if(writer) {
		xmlFreeTextWriter(writer);
	}
	if(buf) {
		xmlBufferFree(buf);
	}
	return rc;
}

int eid_vwr_serialize(void* data) {
	const struct eid_vwr_cache_item* item = cache_get_data("xml");
	FILE* f = fopen((const char*)data, "w");
	fwrite(item->data, item->len, 1, f);
	return fclose(f);
}

static int read_elements(xmlTextReaderPtr reader, struct element_desc* element) {
	int rc;
	void* val = NULL;
	while((rc = xmlTextReaderRead(reader)) > 0) {
		const xmlChar *curnode = xmlTextReaderConstLocalName(reader);
		struct element_desc *desc = get_elemdesc((const char*)curnode);
		struct attribute_desc *att;
		if(xmlTextReaderNodeType(reader) != XML_READER_TYPE_ELEMENT) {
			continue;
		}
		if(xmlTextReaderHasAttributes(reader) > 0) {
			if(desc->attributes == NULL) {
				be_log(EID_VWR_LOG_ERROR, "Could not read file: found attribute on an element that shouldn't have one.");
				return -1;
			}
			for(att = desc->attributes; att->name != NULL; att++) {
				xmlChar* value = xmlTextReaderGetAttribute(reader, att->name);
				if(value) {
					int len;
					val = convert_from_xml(att->label, value, &len);
					cache_add(att->label, val, len);
					eid_vwr_p11_to_ui(att->label, val, len);
					val = NULL;
					xmlFree(value);
				} else {
					if(att->reqd) {
						be_log(EID_VWR_LOG_ERROR, "Could not read file: missing attribute %s on %s", att->name, desc->name);
						return -1;
					}
				}
			}
		}
		if(desc->label != NULL) {
			int len;
			check_xml(xmlTextReaderRead(reader));
			if(desc->is_b64) {
				const char* tmp;
				base64_decodestate state;
				base64_init_decodestate(&state);
				tmp = xmlTextReaderConstValue(reader);
				len = strlen(tmp);
				val = malloc(len);
				len = base64_decode_block(tmp, len, val, &state);
			} else {
				val = convert_from_xml(desc->label, xmlTextReaderConstValue(reader), &len);
			}
			cache_add(desc->label, val, len);
			eid_vwr_p11_to_ui(desc->label, val, len);
			be_log(EID_VWR_LOG_DETAIL, "found data for label %s", desc->label);
			val = NULL;
		}
	}
	if(rc > 0) {
		rc=0;
	}
out:
	if(val != NULL) {
		free(val);
	}
	return rc;
}

int eid_vwr_deserialize(void* data) {
	xmlTextReaderPtr reader = NULL;
	const char* filename = (const char*)data;
	int rc;

	reader = xmlNewTextReaderFilename(filename);
	if(reader == NULL) {
		be_log(EID_VWR_LOG_ERROR, "Could not open file");
		return -1;
	}

	be_newsource(EID_VWR_SRC_FILE);

	check_xml(xmlTextReaderSchemaValidate(reader, get_xsdloc()));
	check_xml(read_elements(reader, toplevel));
	check_xml(eid_vwr_gen_xml(NULL));
out:
	if(rc) {
		xmlError* err = xmlGetLastError();
		if(err != NULL) {
			be_log(EID_VWR_LOG_ERROR, "Could not read file: %s", err->message);
		}
	}
	if(reader) {
		xmlFreeTextReader(reader);
	}
	return rc;
}
