#ifndef STM32_MCP_TOOLS_H
#define STM32_MCP_TOOLS_H

#include "mcp_server.h"

class Stm32McpTools {
public:
    static void RegisterTools(McpServer& mcp_server);
};

#endif // STM32_MCP_TOOLS_H
