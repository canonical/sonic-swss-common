/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "error.h"
#include "logger.h"
#include "errormap.h"

namespace swss {

    ErrorMap::~ErrorMap() { }

    const ErrorMap::SwssStrToRCMap ErrorMap::m_swssStrToRC = {
        { std::make_pair("SWSS_RC_SUCCESS",             SWSS_RC_SUCCESS)         },
        { std::make_pair("SWSS_RC_INVALID_PARAM",       SWSS_RC_INVALID_PARAM)   },
        { std::make_pair("SWSS_RC_UNAVAIL",             SWSS_RC_UNAVAIL)         },
        { std::make_pair("SWSS_RC_NOT_FOUND",           SWSS_RC_NOT_FOUND)       },
        { std::make_pair("SWSS_RC_NO_MEMORY",           SWSS_RC_NO_MEMORY)       },
        { std::make_pair("SWSS_RC_EXISTS",              SWSS_RC_EXISTS)          },
        { std::make_pair("SWSS_RC_TABLE_FULL",          SWSS_RC_TABLE_FULL)      },
        { std::make_pair("SWSS_RC_IN_USE",              SWSS_RC_IN_USE)          },
        { std::make_pair("SWSS_RC_NOT_IMPLEMENTED",     SWSS_RC_NOT_IMPLEMENTED) },
        { std::make_pair("SWSS_RC_FAILURE",             SWSS_RC_FAILURE)         }
    };

    const ErrorMap::SaiToSwssRCMap ErrorMap::m_saiToSwssRC = {
        { "SAI_STATUS_SUCCESS",             "SWSS_RC_SUCCESS"           },
        { "SAI_STATUS_INVALID_PARAMETER",   "SWSS_RC_INVALID_PARAM"     },
        { "SAI_STATUS_NOT_SUPPORTED",       "SWSS_RC_UNAVAIL"           },
        { "SAI_STATUS_ITEM_NOT_FOUND",      "SWSS_RC_NOT_FOUND"         },
        { "SAI_STATUS_NO_MEMEORY",          "SWSS_RC_NO_MEMORY"         },
        { "SAI_STATUS_ITEM_ALREADY_EXISTS", "SWSS_RC_EXISTS"            },
        { "SAI_STATUS_TABLE_FULL",          "SWSS_RC_TABLE_FULL"        },
        { "SAI_STATUS_OBJECT_IN_USE",       "SWSS_RC_IN_USE"            },
        { "SAI_STATUS_NOT_IMPLEMENTED",     "SWSS_RC_NOT_IMPLEMENTED"   },
        { "SAI_STATUS_FAILURE",             "SWSS_RC_FAILURE"           }
    };

    ErrorMap &ErrorMap::getInstance()
    {
        static ErrorMap m_errorMap;
        return m_errorMap;
    }

    std::string ErrorMap::getSwssRCStr(const std::string &saiRCStr)
    {
        std::string swssRCStr;

        if (m_saiToSwssRC.find(saiRCStr) == m_saiToSwssRC.end())
        {
            SWSS_LOG_ERROR("Failed to map SAI error %s to SWSS error code", saiRCStr.c_str());
            swssRCStr = "SWSS_RC_UNKNOWN";
        }
        else
        {
            swssRCStr = m_saiToSwssRC.at(saiRCStr);
        }
        return swssRCStr;
    }

    ErrorMap::SwssRC ErrorMap::getSwssRC(const std::string &swssRCStr)
    {
        for (auto &elem : m_swssStrToRC)
        {
            if (elem.first == swssRCStr)
            {
                return elem.second;
                break;
            }
        }

        /* Error mapping is not found */
        SWSS_LOG_ERROR("Invalid SWSS error string %s is received", swssRCStr.c_str());
        return SWSS_RC_UNKNOWN;
    }

    std::string ErrorMap::getSaiRCStr(SwssRC rc)
    {
        for (auto &elem : m_swssStrToRC)
        {
            if (elem.second == rc)
            {
                return elem.first;
            }
        }

        /* Error mapping is not found */
        SWSS_LOG_ERROR("Invalid SWSS error code %d is received", rc);

        return "SWSS_RC_UNKNOWN";
    }

};
