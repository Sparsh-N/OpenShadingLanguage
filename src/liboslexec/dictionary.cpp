// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include <OpenImageIO/strutil.h>

#include <pugixml.hpp>

#ifdef USING_OIIO_PUGI
namespace pugi = OIIO::pugi;
#endif


#include "oslexec_pvt.h"
#include <OSL/fmt_util.h>

OSL_NAMESPACE_BEGIN

namespace pvt {  // OSL::pvt


// Helper class to manage the dictionaries.
//
// Shaders are written as if they parse arbitrary things from whole
// cloth on every call: from potentially loading XML from disk, parsing
// it, doing queries, and converting the string data to other types.
//
// But that is expensive, so we really cache all this stuff at several
// levels.
//
// We have parsed xml (as pugi::xml_document *'s) cached in a hash table,
// looked up by the xml and/or dictionary name.  Either will do, if it
// looks like a filename, it will read the XML from the file, otherwise it
// will interpret it as xml directly.
//
// Also, individual queries are cached in a hash table.  The key is a
// tuple of (nodeID, query_string, type_requested), so that asking for a
// particular query to return a string is a totally different cache
// entry than asking for it to be converted to a matrix, say.
//
class Dictionary {
public:
    Dictionary(ShadingContext* ctx) : m_context(ctx)
    {
        // Create placeholder element 0 == 'not found'
        m_nodes.emplace_back(0, pugi::xml_node());
    }
    ~Dictionary()
    {
        // Free all the documents.
        for (auto& doc : m_documents)
            delete doc;
    }

    int dict_find(ExecContextPtr ec, ustring dictionaryname, ustring query);
    int dict_find(ExecContextPtr ec, int nodeID, ustring query);
    int dict_next(int nodeID);
    int dict_value(int nodeID, ustring attribname, TypeDesc type, void* data,
                   bool treat_ustrings_as_hash);

private:
    // We cache individual queries with a key that is a tuple of the
    // (nodeID, query_string, type_requested).
    struct Query {
        int document;   // which dictionary document
        int node;       // root node for the search
        ustring name;   // name for the the search
        TypeDesc type;  // UNKNOWN signifies a node, versus an attribute value
        Query(int doc_, int node_, ustring name_,
              TypeDesc type_ = TypeDesc::UNKNOWN)
            : document(doc_), node(node_), name(name_), type(type_)
        {
        }
        bool operator==(const Query& q) const
        {
            return document == q.document && node == q.node && name == q.name
                   && type == q.type;
        }
    };

    // Must define a hash operation to build the unordered_map.
    struct QueryHash {
        size_t operator()(const Query& key) const
        {
            return key.name.hash() + 17 * key.node + 79 * key.document;
        }
    };

    // The cached query result is mostly just a 'valueoffset', which is
    // the index into floatdata/intdata/stringdata (depending on the type
    // being asked for) at which the decoded data live, or a node ID
    // if the query was for a node rather than for an attribute.
    struct QueryResult {
        int valueoffset;  // Offset into one of the 'data' vectors, or nodeID
        bool is_valid;    // true: query found
        QueryResult(bool valid = true) : valueoffset(0), is_valid(valid) {}
        QueryResult(bool /*isnode*/, int value)
            : valueoffset(value), is_valid(true)
        {
        }
    };

    // Nodes we've looked up.  Includes a 'next' index of the matching node
    // for the query that generated this one.
    struct Node {
        int document;         // which document the node belongs to
        pugi::xml_node node;  // which node within the dictionary
        int next;             // next node for the same query
        Node(int d, const pugi::xml_node& n) : document(d), node(n), next(0) {}
    };

    typedef std::unordered_map<Query, QueryResult, QueryHash> QueryMap;
    typedef std::unordered_map<ustring, int> DocMap;

    ShadingContext* m_context;  // back-pointer to shading context

    // List of XML documents we've read in.
    std::vector<pugi::xml_document*> m_documents;

    // Map xml strings and/or filename to indices in m_documents.
    DocMap m_document_map;

    // Cache of fully resolved queries.
    Dictionary::QueryMap m_cache;  // query cache

    // List of all the nodes we've found by queries.
    std::vector<Dictionary::Node> m_nodes;

    // m_floatdata, m_intdata, and m_stringdata hold the decoded data
    // results (including type conversion) of cached queries.
    std::vector<float> m_floatdata;
    std::vector<int> m_intdata;
    std::vector<ustring> m_stringdata;

    // Helper function: return the document index given dictionary name.
    int get_document_index(ExecContextPtr ec, ustring dictionaryname);
};



int
Dictionary::get_document_index(ExecContextPtr ec, ustring dictionaryname)
{
    DocMap::iterator dm = m_document_map.find(dictionaryname);
    int dindex;
    if (dm == m_document_map.end()) {
        dindex                         = m_documents.size();
        m_document_map[dictionaryname] = dindex;
        pugi::xml_document* doc        = new pugi::xml_document;
        m_documents.push_back(doc);
        pugi::xml_parse_result parse_result;
        if (Strutil::ends_with(dictionaryname, ".xml")) {
            // xml file -- read it
            parse_result = doc->load_file(dictionaryname.c_str());
        } else {
            // load xml directly from the string
            parse_result = doc->load_string(dictionaryname.c_str());
        }
        if (!parse_result) {
            // Batched case doesn't support error customization yet,
            // so continue to report through the context when ec is null
            if (ec == nullptr) {
                m_context->errorfmt("XML parsed with errors: {}, at offset {}",
                                    parse_result.description(),
                                    parse_result.offset);
            } else {
                OSL::errorfmt(ec, "XML parsed with errors: {}, at offset {}",
                              parse_result.description(), parse_result.offset);
            }
            m_document_map[dictionaryname] = -1;
            return -1;
        }
    } else {
        dindex = dm->second;
    }

    OSL_DASSERT(dindex < (int)m_documents.size());
    return dindex;
}



int
Dictionary::dict_find(ExecContextPtr ec, ustring dictionaryname, ustring query)
{
    int dindex = get_document_index(ec, dictionaryname);
    if (dindex < 0)
        return dindex;

    Query q(dindex, 0, query);
    QueryMap::iterator qfound = m_cache.find(q);
    if (qfound != m_cache.end()) {
        return qfound->second.valueoffset;
    }

    pugi::xml_document* doc = m_documents[dindex];

    // Query was not found.  Do the expensive lookup and cache it
    pugi::xpath_node_set matches;

    try {
        matches = doc->select_nodes(query.c_str());
    } catch (const pugi::xpath_exception& e) {
        // Batched case doesn't support error customization yet,
        // so continue to report through the context when ec is null
        if (ec == nullptr) {
            m_context->errorfmt("Invalid dict_find query '{}': {}", query,
                                e.what());
        } else {
            OSL::errorfmt(ec, "Invalid dict_find query '{}': {}", query,
                          e.what());
        }
        return 0;
    }

    if (matches.empty()) {
        m_cache[q] = QueryResult(false);  // mark invalid
        return 0;                         // Not found
    }
    int firstmatch = (int)m_nodes.size();
    int last       = -1;
    for (auto&& m : matches) {
        m_nodes.emplace_back(dindex, m.node());
        int nodeid = (int)m_nodes.size() - 1;
        if (last < 0) {
            // If this is the first match, add a cache entry for it
            m_cache[q] = QueryResult(true /* it's a node */, nodeid);
        } else {
            // If this is a subsequent match, set the last match's 'next'
            m_nodes[last].next = nodeid;
        }
        last = nodeid;
    }
    return firstmatch;
}



int
Dictionary::dict_find(ExecContextPtr ec, int nodeID, ustring query)
{
    if (nodeID <= 0 || nodeID >= (int)m_nodes.size())
        return 0;  // invalid node ID

    int document = m_nodes[nodeID].document;
    Query q(document, nodeID, query);
    QueryMap::iterator qfound = m_cache.find(q);
    if (qfound != m_cache.end()) {
        return qfound->second.valueoffset;
    }

    // Query was not found.  Do the expensive lookup and cache it
    pugi::xpath_node_set matches;
    try {
        matches = m_nodes[nodeID].node.select_nodes(query.c_str());
    } catch (const pugi::xpath_exception& e) {
        // Batched case doesn't support error customization yet,
        // so continue to report through the context when ec is null
        if (ec == nullptr) {
            m_context->errorfmt("Invalid dict_find query '{}': {}", query,
                                e.what());
        } else {
            OSL::errorfmt(ec, "Invalid dict_find query '{}': {}", query,
                          e.what());
        }
        return 0;
    }

    if (matches.empty()) {
        m_cache[q] = QueryResult(false);  // mark invalid
        return 0;                         // Not found
    }
    int firstmatch = (int)m_nodes.size();
    int last       = -1;
    for (auto&& m : matches) {
        m_nodes.emplace_back(document, m.node());
        int nodeid = (int)m_nodes.size() - 1;
        if (last < 0) {
            // If this is the first match, add a cache entry for it
            m_cache[q] = QueryResult(true /* it's a node */, nodeid);
        } else {
            // If this is a subsequent match, set the last match's 'next'
            m_nodes[last].next = nodeid;
        }
        last = nodeid;
    }
    return firstmatch;
}



int
Dictionary::dict_next(int nodeID)
{
    if (nodeID <= 0 || nodeID >= (int)m_nodes.size())
        return 0;  // invalid node ID
    return m_nodes[nodeID].next;
}



int
Dictionary::dict_value(int nodeID, ustring attribname, TypeDesc type,
                       void* data, bool treat_ustrings_as_hash)
{
    if (nodeID <= 0 || nodeID >= (int)m_nodes.size())
        return 0;  // invalid node ID

    const Dictionary::Node& node(m_nodes[nodeID]);
    Dictionary::Query q(node.document, nodeID, attribname, type);
    Dictionary::QueryMap::iterator qfound = m_cache.find(q);
    if (qfound != m_cache.end()) {
        // previously found
        int offset = qfound->second.valueoffset;
        int n      = type.numelements() * type.aggregate;
        if (type.basetype == TypeDesc::STRING) {
            OSL_DASSERT(n == 1 && "no string arrays in XML");
            if (treat_ustrings_as_hash == true) {
                ((ustringhash_pod*)data)[0] = m_stringdata[offset].hash();
            } else {
                ((ustring*)data)[0] = m_stringdata[offset];
            }
            return 1;
        }
        if (type.basetype == TypeDesc::INT) {
            for (int i = 0; i < n; ++i)
                ((int*)data)[i] = m_intdata[offset++];
            return 1;
        }
        if (type.basetype == TypeDesc::FLOAT) {
            for (int i = 0; i < n; ++i)
                ((float*)data)[i] = m_floatdata[offset++];
            return 1;
        }
        return 0;  // Unknown type
    }

    // OK, the entry wasn't in the cache, we need to decode it and cache it.

    const char* val = NULL;
    if (attribname.empty()) {
        val = node.node.value();
    } else {
        for (pugi::xml_attribute_iterator ait = node.node.attributes_begin();
             ait != node.node.attributes_end(); ++ait) {
            if (ait->name() == attribname) {
                val = ait->value();
                break;
            }
        }
    }
    if (val == NULL)
        return 0;  // not found

    Dictionary::QueryResult r(false, 0);
    int n = type.numelements() * type.aggregate;
    if (type.basetype == TypeDesc::STRING && n == 1) {
        r.valueoffset = (int)m_stringdata.size();
        ustring s(val);
        m_stringdata.push_back(s);
        if (treat_ustrings_as_hash == true) {
            ((ustringhash_pod*)data)[0] = s.hash();
        } else {
            ((ustring*)data)[0] = s;
        }
        m_cache[q] = r;
        return 1;
    }
    if (type.basetype == TypeDesc::INT) {
        r.valueoffset = (int)m_intdata.size();
        string_view valstr(val);
        for (int i = 0; i < n; ++i) {
            int v;
            OIIO::Strutil::parse_int(valstr, v);
            OIIO::Strutil::parse_char(valstr, ',');
            m_intdata.push_back(v);
            ((int*)data)[i] = v;
        }
        m_cache[q] = r;
        return 1;
    }
    if (type.basetype == TypeDesc::FLOAT) {
        r.valueoffset = (int)m_floatdata.size();
        string_view valstr(val);
        for (int i = 0; i < n; ++i) {
            float v;
            OIIO::Strutil::parse_float(valstr, v);
            OIIO::Strutil::parse_char(valstr, ',');
            m_floatdata.push_back(v);
            ((float*)data)[i] = v;
        }
        m_cache[q] = r;
        return 1;
    }

    // Anything that's left is an unsupported type
    return 0;
}


};  // namespace pvt



int
ShadingContext::dict_find(ExecContextPtr ec, ustring dictionaryname,
                          ustring query)
{
    if (!m_dictionary) {
        m_dictionary = new Dictionary(this);
    }
    return m_dictionary->dict_find(ec, dictionaryname, query);
}



int
ShadingContext::dict_find(ExecContextPtr ec, int nodeID, ustring query)
{
    if (!m_dictionary) {
        m_dictionary = new Dictionary(this);
    }
    return m_dictionary->dict_find(ec, nodeID, query);
}



int
ShadingContext::dict_next(int nodeID)
{
    if (!m_dictionary)
        return 0;
    return m_dictionary->dict_next(nodeID);
}



int
ShadingContext::dict_value(int nodeID, ustring attribname, TypeDesc type,
                           void* data, bool treat_ustrings_as_hash)
{
    if (!m_dictionary)
        return 0;
    return m_dictionary->dict_value(nodeID, attribname, type, data,
                                    treat_ustrings_as_hash);
}



void
ShadingContext::free_dict_resources()
{
    delete m_dictionary;
}



OSL_SHADEOP int
osl_dict_find_iis(OpaqueExecContextPtr oec, int nodeID, ustringhash_pod query_)
{
    auto ec    = pvt::get_ec(oec);
    auto query = ustringhash_from(query_);
    return ec->context->dict_find(ec, nodeID, ustring_from(query));
}



OSL_SHADEOP int
osl_dict_find_iss(OpaqueExecContextPtr oec, ustringhash_pod dictionary_,
                  ustringhash_pod query_)
{
    auto dictionary = ustringhash_from(dictionary_);
    auto query      = ustringhash_from(query_);
    auto ec         = pvt::get_ec(oec);
    return ec->context->dict_find(ec, ustring_from(dictionary),
                                  ustring_from(query));
}



OSL_SHADEOP int
osl_dict_next(OpaqueExecContextPtr oec, int nodeID)
{
    auto ec = pvt::get_ec(oec);
    return ec->context->dict_next(nodeID);
}



OSL_SHADEOP int
osl_dict_value(OpaqueExecContextPtr oec, int nodeID,
               ustringhash_pod attribname_, long long type, void* data)
{
    auto ec         = pvt::get_ec(oec);
    auto attribname = ustringhash_from(attribname_);
    return ec->context->dict_value(nodeID, ustring_from(attribname),
                                   TYPEDESC(type), data, true);
}



OSL_NAMESPACE_END
