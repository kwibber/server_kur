#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================== RAII ОБЕРТКИ ДЛЯ open62541 ==============================
class UAString {
private:
    UA_String str;
    bool ownsMemory;
    
public:
    UAString(const char* cstr) : ownsMemory(true) {
        str.length = strlen(cstr);
        str.data = (UA_Byte*)UA_malloc(str.length);
        if (str.data) {
            memcpy(str.data, cstr, str.length);
        } else {
            str.length = 0;
            ownsMemory = false;
        }
    }
    
    // Запрещаем копирование
    UAString(const UAString&) = delete;
    UAString& operator=(const UAString&) = delete;
    
    // Разрешаем перемещение
    UAString(UAString&& other) noexcept : str(other.str), ownsMemory(other.ownsMemory) {
        other.ownsMemory = false;
        other.str.data = nullptr;
        other.str.length = 0;
    }
    
    ~UAString() {
        if (ownsMemory) {
            UA_String_clear(&str);
        }
    }
    
    UA_String* get() { return &str; }
    const UA_String* get() const { return &str; }
};

class UALocalizedText {
private:
    UA_LocalizedText text;
    
public:
    UALocalizedText(const char* locale, const char* txt) {
        text.locale = UA_STRING_ALLOC(locale);
        text.text = UA_STRING_ALLOC(txt);
    }
    
    ~UALocalizedText() {
        UA_LocalizedText_clear(&text);
    }
    
    // Запрещаем копирование
    UALocalizedText(const UALocalizedText&) = delete;
    UALocalizedText& operator=(const UALocalizedText&) = delete;
    
    // Разрешаем перемещение
    UALocalizedText(UALocalizedText&& other) noexcept : text(other.text) {
        // Обнуляем у перемещенного объекта, чтобы деструктор не освободил память
        other.text.locale.data = nullptr;
        other.text.locale.length = 0;
        other.text.text.data = nullptr;
        other.text.text.length = 0;
    }
    
    UA_LocalizedText* get() { return &text; }
    const UA_LocalizedText* get() const { return &text; }
};

class UAQualifiedName {
private:
    UA_QualifiedName name;
    
public:
    UAQualifiedName(UA_UInt16 nsIndex, const char* nameStr) {
        name.namespaceIndex = nsIndex;
        name.name = UA_STRING_ALLOC(nameStr);
    }
    
    ~UAQualifiedName() {
        UA_QualifiedName_clear(&name);
    }
    
    // Запрещаем копирование
    UAQualifiedName(const UAQualifiedName&) = delete;
    UAQualifiedName& operator=(const UAQualifiedName&) = delete;
    
    // Разрешаем перемещение
    UAQualifiedName(UAQualifiedName&& other) noexcept : name(other.name) {
        // Обнуляем у перемещенного объекта
        other.name.name.data = nullptr;
        other.name.name.length = 0;
    }
    
    UA_QualifiedName* get() { return &name; }
    const UA_QualifiedName* get() const { return &name; }
};

// ============================== БАЗОВЫЙ КЛАСС УЗЛА ==============================
class OPCUANode {
protected:
    UA_Server* server;
    UA_NodeId nodeId;
    
public:
    OPCUANode(UA_Server* srv, const UA_NodeId& id) 
        : server(srv), nodeId(id) {}
    
    virtual ~OPCUANode() = default;
    
    // Запрещаем копирование
    OPCUANode(const OPCUANode&) = delete;
    OPCUANode& operator=(const OPCUANode&) = delete;
    
    UA_NodeId getNodeId() const { return nodeId; }
    UA_Server* getServer() const { return server; }
    
    virtual void initialize() = 0;
};

// ============================== КЛАСС ПЕРЕМЕННОЙ ==============================
class OPCUAVariable : public OPCUANode {
protected:
    std::string displayName;
    std::string description;
    std::string browseName;
    double initialValue;
    
public:
    OPCUAVariable(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id, 
                  const std::string& browseName, const std::string& displayName,
                  const std::string& description, double initialValue)
        : OPCUANode(srv, UA_NODEID_NUMERIC(nsIndex, id)),
          displayName(displayName), 
          description(description),
          browseName(browseName),
          initialValue(initialValue) {}
    
    virtual void initialize() override {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        
        // Создаем локализованные строки
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        
        // Устанавливаем начальное значение
        UA_Variant_setScalarCopy(&attr.value, &initialValue, &UA_TYPES[UA_TYPES_DOUBLE]);
        
        // Добавляем как переменную в ObjectsFolder
        UA_StatusCode status = UA_Server_addVariableNode(server, nodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, NULL, NULL);
        
        // Очищаем значение, так как оно было скопировано
        UA_Variant_clear(&attr.value);
    }
    
    void writeValue(double value) {
        UA_Variant var;
        UA_Variant_init(&var);
        UA_Variant_setScalarCopy(&var, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        UA_Server_writeValue(server, nodeId, var);
        UA_Variant_clear(&var);
    }
};

// ============================== КЛАСС ПЕРЕМЕННОЙ В КАЧЕСТВЕ КОМПОНЕНТА ==============================
class OPCUAComponentVariable : public OPCUAVariable {
private:
    UA_NodeId parentNodeId;
    
public:
    OPCUAComponentVariable(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id,
                          const std::string& browseName, const std::string& displayName,
                          const std::string& description, double initialValue,
                          const UA_NodeId& parentId)
        : OPCUAVariable(srv, nsIndex, id, browseName, displayName, description, initialValue),
          parentNodeId(parentId) {}
    
    void initialize() override {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        
        UA_Variant_setScalarCopy(&attr.value, &initialValue, &UA_TYPES[UA_TYPES_DOUBLE]);
        
        // Добавляем как компонент родительского узла
        UA_StatusCode status = UA_Server_addVariableNode(server, nodeId,
            parentNodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, NULL, NULL);
        
        // Очищаем значение
        UA_Variant_clear(&attr.value);
    }
};

// ============================== КЛАСС УСТРОЙСТВА ==============================
class OPCUADevice : public OPCUANode {
private:
    std::string displayName;
    std::string description;
    std::string browseName;
    std::vector<std::unique_ptr<OPCUAComponentVariable>> components;
    
public:
    OPCUADevice(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id,
                const std::string& browseName, const std::string& displayName,
                const std::string& description)
        : OPCUANode(srv, UA_NODEID_NUMERIC(nsIndex, id)),
          displayName(displayName),
          description(description),
          browseName(browseName) {}
    
    void initialize() override {
        UA_ObjectAttributes attr = UA_ObjectAttributes_default;
        
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        
        UA_StatusCode status = UA_Server_addObjectNode(
            server, nodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
            attr, NULL, NULL);
        
        // Инициализируем все компоненты
        for (auto& component : components) {
            if (component) {
                component->initialize();
            }
        }
    }
    
    void addComponent(std::unique_ptr<OPCUAComponentVariable> component) {
        components.push_back(std::move(component));
    }
};

// ============================== КЛАСС МУЛЬТИМЕТРА ==============================
class Multimeter : public OPCUADevice {
private:
    OPCUAComponentVariable* voltage;
    OPCUAComponentVariable* current;
    std::mt19937 rng;
    
public:
    Multimeter(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 100, "Multimeter", "Multimeter", 
                     "Electrical measurement device"),
          rng(std::random_device{}()),
          voltage(nullptr),
          current(nullptr) {
        
        // Создаем компоненты мультиметра
        auto voltageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 101, "Voltage", "Voltage", 
            "Measured voltage (Volts)", 220.0, nodeId);
        
        auto currentVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 102, "Current", "Current", 
            "Measured current (Amperes)", 5.0, nodeId);
        
        voltage = voltageVar.get();
        current = currentVar.get();
        
        addComponent(std::move(voltageVar));
        addComponent(std::move(currentVar));
    }
    
    void updateRandomValues() {
        std::uniform_real_distribution<double> voltageDist(190.0, 240.0);
        std::uniform_real_distribution<double> currentDist(0.5, 15.0);
        
        double v = voltageDist(rng);
        double c = currentDist(rng);
        
        if (voltage) voltage->writeValue(v);
        if (current) current->writeValue(c);
        
        std::cout << "\rMultimeter: Voltage = " << v << " V, Current = " << c << " A" << std::flush;
    }
};

// ============================== КЛАСС СЕРВЕРА OPC UA ==============================
class OPCUAServer {
private:
    UA_Server* server;
    UA_UInt16 namespaceIndex;
    std::atomic<bool> running;
    std::unique_ptr<Multimeter> multimeter;
    std::unique_ptr<OPCUAVariable> flywheelRPM;
    
public:
    OPCUAServer() : server(nullptr), namespaceIndex(0), running(true) {
        initConsole();
    }
    
    ~OPCUAServer() {
        stop();
    }
    
    // Запрещаем копирование
    OPCUAServer(const OPCUAServer&) = delete;
    OPCUAServer& operator=(const OPCUAServer&) = delete;
    
    bool initialize() {
        std::cout << "OPC UA Server initializing..." << std::endl;
        
        // Создаем сервер
        server = UA_Server_new();
        if (!server) {
            std::cerr << "Failed to create server" << std::endl;
            return false;
        }
        
        // Настраиваем конфигурацию
        UA_ServerConfig* config = UA_Server_getConfig(server);
        UA_ServerConfig_setDefault(config);
        
        // Добавляем пространство имен
        namespaceIndex = UA_Server_addNamespace(server, "MyNamespace");
        
        // Создаем устройства
        multimeter = std::make_unique<Multimeter>(server, namespaceIndex);
        multimeter->initialize();
        
        // Создаем переменную "Обороты маховика"
        flywheelRPM = std::make_unique<OPCUAVariable>(
            server, namespaceIndex, 3, "FlywheelRPM", "Flywheel RPM",
            "Flywheel rotation speed (RPM)", 0.0);
        flywheelRPM->initialize();
        
        return true;
    }
    
    bool start() {
        std::cout << "\nServer running on opc.tcp://localhost:4840" << std::endl;
        std::cout << "Device structure:" << std::endl;
        std::cout << "  - Multimeter (ID: ns=" << namespaceIndex << ";i=100)" << std::endl;
        std::cout << "    ├── Voltage (ID: ns=" << namespaceIndex << ";i=101)" << std::endl;
        std::cout << "    └── Current (ID: ns=" << namespaceIndex << ";i=102)" << std::endl;
        std::cout << "  - Flywheel RPM (ID: ns=" << namespaceIndex << ";i=3)" << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        // Запускаем сервер
        UA_StatusCode status = UA_Server_run_startup(server);
        if (status != UA_STATUSCODE_GOOD) {
            std::cerr << "Failed to start server: " << UA_StatusCode_name(status) << std::endl;
            return false;
        }
        
        return true;
    }
    
    void run() {
        while (running) {
            // Обновляем значения мультиметра
            if (multimeter) {
                multimeter->updateRandomValues();
            }
            
            // Обрабатываем сетевые события
            UA_Server_run_iterate(server, false);
            
            // Короткая пауза
            std::this_thread::sleep_for(std::chrono::milliseconds(330));
        }
    }
    
    void stop() {
        if (!running) return; // Уже остановлен
        
        running = false;
        
        if (server) {
            std::cout << "\nStopping server..." << std::endl;
            
            // ВАЖНО: Сначала очищаем все узлы, которые ссылаются на сервер
            flywheelRPM.reset();
            multimeter.reset();
            
            // Затем останавливаем и удаляем сервер
            UA_Server_run_shutdown(server);
            UA_Server_delete(server);
            server = nullptr;
            
            std::cout << "Server stopped." << std::endl;
        }
    }
    
private:
    void initConsole() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    }
};

// ============================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ ОБРАБОТКИ СИГНАЛОВ ==============================
std::atomic<bool> globalRunning(true);

void signalHandler(int signal) {
    (void)signal;
    std::cout << "\nSignal received, stopping server..." << std::endl;
    globalRunning = false;
}

// ============================== ТОЧКА ВХОДА ==============================
int main() {
    // Инициализация консоли
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    
    std::cout << "OPC UA Server starting..." << std::endl;
    
    // Устанавливаем обработчики сигналов
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    try {
        // Создаем и запускаем сервер
        OPCUAServer server;
        
        if (!server.initialize()) {
            return 1;
        }
        
        if (!server.start()) {
            return 1;
        }
        
        // Основной цикл в отдельном потоке
        std::thread serverThread([&server]() {
            server.run();
        });
        
        // Ждем сигнала остановки в основном потоке
        while (globalRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Останавливаем сервер
        server.stop();
        
        // Ждем завершения потока сервера
        if (serverThread.joinable()) {
            serverThread.join();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Server terminated successfully." << std::endl;
    return 0;
}