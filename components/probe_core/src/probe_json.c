#include "probe_json.h"

#include "cJSON.h"

char *probe_json_print_and_delete(cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}
