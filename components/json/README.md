# json compatibility component

ESP-IDF 6 removed the built-in `json` component. Some managed components still
refer to that legacy component name. This local component keeps that name
available and forwards the dependency to `espressif/cjson`.
