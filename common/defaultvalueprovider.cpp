#include <boost/algorithm/string.hpp>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h> 
#include <stdio.h> 

#include <libyang/libyang.h>

#include "defaultvalueprovider.h"
#include "logger.h"

using namespace std;
using namespace swss;

#ifdef DEBUG
#include <execinfo.h>
#include <signal.h>
#define TRACE_STACK_SIZE 30
[[noreturn]] void sigv_handler(int sig) {
  void *stackInfo[TRACE_STACK_SIZE];
  size_t stackInfoSize = backtrace(stackInfo, TRACE_STACK_SIZE);

  // print out all the frames to stderr
  cerr << "Error: signal " << sig << ":\n" << endl;
  backtrace_symbols_fd(stackInfo, (int)stackInfoSize, STDERR_FILENO);
  exit(1);
}
#endif

[[noreturn]] void ThrowRunTimeError(string message)
{
    SWSS_LOG_ERROR("DefaultValueProvider: %s", message.c_str());
    throw std::runtime_error(message);
}

TableInfoBase::TableInfoBase()
{
    // C++ need this empty ctor
}

std::shared_ptr<std::string> TableInfoBase::GetDefaultValue(std::string row, std::string field)
{
    assert(!row.empty());
    assert(!field.empty());

    SWSS_LOG_DEBUG("TableInfoBase::GetDefaultValue %s %s\n", row.c_str(), field.c_str());
    FieldDefaultValueMapping *fieldMappingPtr;
    if (!FindFieldMappingByKey(row, &fieldMappingPtr)) {
        SWSS_LOG_DEBUG("Can't found default value mapping for row %s\n", row.c_str());
        return nullptr;
    }

    auto fieldData = fieldMappingPtr->find(field);
    if (fieldData == fieldMappingPtr->end())
    {
        SWSS_LOG_DEBUG("Can't found default value for field %s\n", field.c_str());
        return nullptr;
    }

    SWSS_LOG_DEBUG("Found default value for field %s=%s\n", field.c_str(), fieldData->second.c_str());
    return std::make_shared<std::string>(fieldData->second);
}

// existedValues and targetValues can be same container.
void TableInfoBase::AppendDefaultValues(string row, FieldValueMapping& existedValues, FieldValueMapping& targetValues)
{
    assert(!row.empty());

    SWSS_LOG_DEBUG("TableInfoBase::AppendDefaultValues %s\n", row.c_str());
    FieldDefaultValueMapping *fieldMappingPtr;
    if (!FindFieldMappingByKey(row, &fieldMappingPtr)) {
        SWSS_LOG_DEBUG("Can't found default value mapping for row %s\n", row.c_str());
        return;
    }

    for (auto &defaultValue : *fieldMappingPtr)
    {
        auto fieldData = existedValues.find(defaultValue.first);
        if (fieldData != existedValues.end())
        {
            // ignore when a field already has value in existedValues
            continue;
        }

        SWSS_LOG_DEBUG("Append default value: %s=%s\n",defaultValue.first.c_str(), defaultValue.second.c_str());
        targetValues.emplace(defaultValue.first, defaultValue.second);
    }
}

TableInfoDict::TableInfoDict(KeyInfoToDefaultValueInfoMapping &fieldInfoMapping)
{
    for (auto& fieldMapping : fieldInfoMapping)
    {
        // KeyInfo.first is key value
        string keyValue = fieldMapping.first.first;
        m_defaultValueMapping.emplace(keyValue, fieldMapping.second);
    }
}

bool TableInfoDict::FindFieldMappingByKey(string row, FieldDefaultValueMapping ** foundedMappingPtr)
{
    assert(!row.empty());
    assert(foundedMappingPtr != nullptr);

    SWSS_LOG_DEBUG("TableInfoDict::FindFieldMappingByKey %s\n", row.c_str());
    auto keyResult = m_defaultValueMapping.find(row);
    *foundedMappingPtr = keyResult->second.get();
    return keyResult == m_defaultValueMapping.end();
}

TableInfoSingleList::TableInfoSingleList(KeyInfoToDefaultValueInfoMapping &fieldInfoMapping)
{
    m_defaultValueMapping = fieldInfoMapping.begin()->second;
}

bool TableInfoSingleList::FindFieldMappingByKey(string row, FieldDefaultValueMapping ** foundedMappingPtr)
{
    assert(!row.empty());
    assert(foundedMappingPtr != nullptr);

    SWSS_LOG_DEBUG("TableInfoSingleList::FindFieldMappingByKey %s\n", row.c_str());
    *foundedMappingPtr = m_defaultValueMapping.get();
    return true;
}

TableInfoMultipleList::TableInfoMultipleList(KeyInfoToDefaultValueInfoMapping &fieldInfoMapping)
{
    for (auto& fieldMapping : fieldInfoMapping)
    {
        // KeyInfo.second is field count
        int fieldCount = fieldMapping.first.second;
        m_defaultValueMapping.emplace(fieldCount, fieldMapping.second);
    }
}

bool TableInfoMultipleList::FindFieldMappingByKey(string row, FieldDefaultValueMapping ** foundedMappingPtr)
{
    assert(!row.empty());
    assert(foundedMappingPtr != nullptr);

    SWSS_LOG_DEBUG("TableInfoMultipleList::FindFieldMappingByKey %s\n", row.c_str());
    int fieldCount = (int)std::count(row.begin(), row.end(), '|') + 1;
    auto keyInfo = m_defaultValueMapping.find(fieldCount);
    
    // when not found, key_info still a valied iterator
    *foundedMappingPtr = keyInfo->second.get();
    
    // return false when not found
    return keyInfo != m_defaultValueMapping.end();
}

DefaultValueProvider& DefaultValueProvider::Instance()
{
    static DefaultValueProvider instance;
    if (instance.m_context == nullptr)
    {
        instance.Initialize();
    }

    return instance;
}

shared_ptr<TableInfoBase> DefaultValueProvider::FindDefaultValueInfo(std::string table)
{
    assert(!table.empty());

    SWSS_LOG_DEBUG("DefaultValueProvider::FindDefaultValueInfo %s\n", table.c_str());
    auto findResult = m_defaultValueMapping.find(table);
    if (findResult == m_defaultValueMapping.end())
    {
        SWSS_LOG_DEBUG("Not found default value info for %s\n", table.c_str());
        return nullptr;
    }
    
    return findResult->second;
}

std::shared_ptr<std::string> DefaultValueProvider::GetDefaultValue(std::string table, std::string row, std::string field)
{
    assert(!table.empty());
    assert(!row.empty());
    assert(!field.empty());

    SWSS_LOG_DEBUG("DefaultValueProvider::GetDefaultValue %s %s %s\n", table.c_str(), row.c_str(), field.c_str());
#ifdef DEBUG
    if (!FeatureEnabledByEnvironmentVariable())
    {
        return nullptr;
    }
#endif

    auto defaultValueInfo = FindDefaultValueInfo(table);
    if (defaultValueInfo == nullptr)
    {
        SWSS_LOG_DEBUG("Not found default value info for %s\n", table.c_str());
        return nullptr;
    }

    return defaultValueInfo->GetDefaultValue(row, field);
}

void DefaultValueProvider::AppendDefaultValues(std::string table, std::string row, std::vector<std::pair<std::string, std::string> > &values)
{
    assert(!table.empty());
    assert(!row.empty());

    SWSS_LOG_DEBUG("DefaultValueProvider::AppendDefaultValues %s %s\n", table.c_str(), row.c_str());
#ifdef DEBUG
    if (!FeatureEnabledByEnvironmentVariable())
    {
        return;
    }
#endif

    auto defaultValueInfo = FindDefaultValueInfo(table);
    if (defaultValueInfo == nullptr)
    {
        SWSS_LOG_DEBUG("Not found default value info for %s\n", table.c_str());
        return;
    }
    
    map<string, string> existedValues;
    map<string, string> defaultValues;
    for (auto& fieldValuePair : values)
    {
        existedValues.emplace(fieldValuePair.first, fieldValuePair.second);
    }

    defaultValueInfo->AppendDefaultValues(row, existedValues, defaultValues);

    for (auto& fieldValuePair : defaultValues)
    {
        values.emplace_back(fieldValuePair.first, fieldValuePair.second);
    }
}

void DefaultValueProvider::AppendDefaultValues(string table, string row, FieldValueMapping& values)
{
    assert(!table.empty());
    assert(!row.empty());

    SWSS_LOG_DEBUG("DefaultValueProvider::AppendDefaultValues %s %s\n", table.c_str(), row.c_str());
#ifdef DEBUG
    if (!FeatureEnabledByEnvironmentVariable())
    {
        return;
    }
#endif

    auto defaultValueInfo = FindDefaultValueInfo(table);
    if (defaultValueInfo == nullptr)
    {
        SWSS_LOG_DEBUG("Not found default value info for %s\n", table.c_str());
        return;
    }

    defaultValueInfo->AppendDefaultValues(row, values, values);
}

DefaultValueProvider::DefaultValueProvider()
{
#ifdef DEBUG
    // initialize crash event handler for debug.
    signal(SIGSEGV, sigv_handler);
#endif
}


DefaultValueProvider::~DefaultValueProvider()
{
    if (m_context)
    {
        // set private_destructor to NULL because no any private data
        ly_ctx_destroy(m_context, NULL);
    }
}

void DefaultValueProvider::Initialize(char* modulePath)
{
    assert(modulePath != nullptr && strlen(modulePath) != 0);
    assert(m_context == nullptr);

    DIR *moduleDir = opendir(modulePath);
    if (!moduleDir)
    {
        ThrowRunTimeError("Open Yang model path " + string(modulePath) + " failed");
    }

    m_context = ly_ctx_new(modulePath, LY_CTX_ALLIMPLEMENTED);
    struct dirent *subDir;
    while ((subDir = readdir(moduleDir)) != NULL)
    {
        if (subDir->d_type == DT_REG)
        {
            SWSS_LOG_DEBUG("file name: %s\n", subDir->d_name);
            string fileName(subDir->d_name);
            int pos = (int)fileName.find(".yang");
            string moduleName = fileName.substr(0, pos);

            const struct lys_module *module = ly_ctx_load_module(
                                                    m_context,
                                                    moduleName.c_str(),
                                                    EMPTY_STR); // Use EMPTY_STR to revision to load the latest revision
            if (module->data == NULL)
            {
                // Not every yang file should contains yang model
                SWSS_LOG_WARN("Yang file %s does not contains model %s.\n", subDir->d_name, moduleName.c_str());
                continue;
            }

            struct lys_node *topLevelNode = module->data;
            while (topLevelNode)
            {
                if (topLevelNode->nodetype != LYS_CONTAINER)
                {
                    SWSS_LOG_DEBUG("ignore top level element %s, tyoe %d\n",topLevelNode->name, topLevelNode->nodetype);
                    // Config DB table schema is defined by top level container
                    topLevelNode = topLevelNode->next;
                    continue;
                }

                SWSS_LOG_DEBUG("top level container: %s\n",topLevelNode->name);
                auto container = topLevelNode->child;
                while (container)
                {
                    SWSS_LOG_DEBUG("container name: %s\n",container->name);

                    AppendTableInfoToMapping(container);
                    container = container->next;
                }

                topLevelNode = topLevelNode->next;
            }
        }
    }
    closedir(moduleDir);
}

std::shared_ptr<KeyInfo> DefaultValueProvider::GetKeyInfo(struct lys_node* tableChildNode)
{
    assert(tableChildNode != nullptr);
    SWSS_LOG_DEBUG("DefaultValueProvider::GetKeyInfo %s\n",tableChildNode->name);

    int keyFieldCount = 0;
    string keyValue = "";
    if (tableChildNode->nodetype == LYS_LIST)
    {
        SWSS_LOG_DEBUG("Child list: %s\n",tableChildNode->name);

        // when a top level container contains list, the key defined by the 'keys' field.
        struct lys_node_list *listNode = (struct lys_node_list*)tableChildNode;
        string key(listNode->keys_str);
        keyFieldCount = (int)std::count(key.begin(), key.end(), '|') + 1;
    }
    else if (tableChildNode->nodetype == LYS_CONTAINER)
    {
        SWSS_LOG_DEBUG("Child container name: %s\n",tableChildNode->name);

        // when a top level container not contains any list, the key is child container name
        keyValue = string(tableChildNode->name);
    }
    else
    {
        SWSS_LOG_DEBUG("Ignore child element: %s\n",tableChildNode->name);
        return nullptr;
    }
    
    return make_shared<KeyInfo>(keyValue, keyFieldCount);
}

FieldDefaultValueMappingPtr DefaultValueProvider::GetDefaultValueInfo(struct lys_node* tableChildNode)
{
    assert(tableChildNode != nullptr);
    SWSS_LOG_DEBUG("DefaultValueProvider::GetDefaultValueInfo %s\n",tableChildNode->name);

    auto field = tableChildNode->child;
    auto fieldMapping = make_shared<FieldDefaultValueMapping>();
    while (field)
    {
        if (field->nodetype == LYS_LEAF)
        {
            SWSS_LOG_DEBUG("leaf field: %s\n",field->name);
            struct lys_node_leaf *leafNode = (struct lys_node_leaf*)field;
            if (leafNode->dflt)
            {
                SWSS_LOG_DEBUG("field: %s, default: %s\n",leafNode->name, leafNode->dflt);
                fieldMapping->emplace(string(leafNode->name), string(leafNode->dflt));
            }
        }
        else if (field->nodetype == LYS_CHOICE)
        {
            SWSS_LOG_DEBUG("choice field: %s\n",field->name);
            struct lys_node_choice *choiceNode = (struct lys_node_choice *)field;
            if (choiceNode->dflt)
            {
                // TODO: convert choice default value to string
                SWSS_LOG_DEBUG("choice field: %s, default: TBD\n",choiceNode->name);
            }
        }
        else if (field->nodetype == LYS_LEAFLIST)
        {
            SWSS_LOG_DEBUG("list field: %s\n",field->name);
            struct lys_node_leaflist *listNode = (struct lys_node_leaflist *)field;
            if (listNode->dflt)
            {
                // TODO: convert list default value to json string
                SWSS_LOG_DEBUG("list field: %s, default: TBD\n",listNode->name);
            }
        }
#ifdef DEBUG
        else
        {
            SWSS_LOG_DEBUG("Field %s with type %d does not support default value\n",field->name, field->nodetype);
        }
#endif

        field = field->next;
    }

    return fieldMapping;
}

int DefaultValueProvider::BuildFieldMappingList(struct lys_node* table, KeyInfoToDefaultValueInfoMapping &fieldInfoMapping)
{
    assert(table != nullptr);

    int childListCount = 0;
    auto nextChild = table->child;
    while (nextChild)
    {
        // get key from schema
        auto keyInfo = GetKeyInfo(nextChild);
        if (keyInfo == nullptr)
        {
            nextChild = nextChild->next;
            continue;
        }
        else if (keyInfo->second != 0)
        {
            // when key field count not 0, it's a list node.
            childListCount++;
        }

        // get field name to default value mappings from schema
        fieldInfoMapping.emplace(*keyInfo, GetDefaultValueInfo(nextChild));

        nextChild = nextChild->next;
    }

    return childListCount;
}

// Load default value info from yang model and append to default value mapping
void DefaultValueProvider::AppendTableInfoToMapping(struct lys_node* table)
{
    assert(table != nullptr);

    SWSS_LOG_DEBUG("DefaultValueProvider::AppendTableInfoToMapping table name: %s\n",table->name);
    KeyInfoToDefaultValueInfoMapping fieldInfoMapping;
    int listCount = BuildFieldMappingList(table, fieldInfoMapping);

    // create container data by list count
    TableInfoBase* tableInfoPtr = nullptr;
    switch (listCount)
    {
        case 0:
        {
            tableInfoPtr = new TableInfoDict(fieldInfoMapping);
        }
        break;

        case 1:
        {
            tableInfoPtr = new TableInfoSingleList(fieldInfoMapping);
        }
        break;

        default:
        {
            tableInfoPtr = new TableInfoMultipleList(fieldInfoMapping);
        }
        break;
    }

    m_defaultValueMapping.emplace(string(table->name), shared_ptr<TableInfoBase>(tableInfoPtr));
}

#ifdef DEBUG
bool DefaultValueProvider::FeatureEnabledByEnvironmentVariable()
{
    const char* showDefault = getenv("CONFIG_DB_DEFAULT_VALUE");
    if (showDefault == nullptr || strcmp(showDefault, "TRUE") != 0)
    {
        // enable feature with "export CONFIG_DB_DEFAULT_VALUE=TRUE"
        SWSS_LOG_DEBUG("enable feature with \"export CONFIG_DB_DEFAULT_VALUE=TRUE\"\n");
        return false;
    }

    return true;
}
#endif