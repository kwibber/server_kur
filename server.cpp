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
#include <cmath>
#include <algorithm>

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
protected:
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
    
    virtual void updateValues() = 0;
};

// ============================== КЛАСС МУЛЬТИМЕТРА ==============================
class Multimeter : public OPCUADevice {
private:
    OPCUAComponentVariable* voltage;
    OPCUAComponentVariable* current;
    OPCUAComponentVariable* resistance;
    OPCUAComponentVariable* power;
    std::mt19937 rng;
    
public:
    Multimeter(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 100, "Multimeter", "Мультиметр", 
                     "Электрический измерительный прибор"),
          rng(std::random_device{}()),
          voltage(nullptr),
          current(nullptr),
          resistance(nullptr),
          power(nullptr) {
        
        // Создаем компоненты мультиметра
        auto voltageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 101, "Voltage", "Напряжение", 
            "Измеренное напряжение (Вольты)", 220.0, nodeId);
        
        auto currentVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 102, "Current", "Сила тока", 
            "Измеренная сила тока (Амперы)", 5.0, nodeId);
        
        auto resistanceVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 103, "Resistance", "Сопротивление", 
            "Измеренное сопротивление (Омы)", 44.0, nodeId);
        
        auto powerVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 104, "Power", "Мощность", 
            "Расчетная мощность (Ватты)", 1100.0, nodeId);
        
        voltage = voltageVar.get();
        current = currentVar.get();
        resistance = resistanceVar.get();
        power = powerVar.get();
        
        addComponent(std::move(voltageVar));
        addComponent(std::move(currentVar));
        addComponent(std::move(resistanceVar));
        addComponent(std::move(powerVar));
    }
    
    void updateValues() override {
        std::uniform_real_distribution<double> voltageDist(190.0, 240.0);
        std::uniform_real_distribution<double> currentDist(0.5, 15.0);
        
        double v = voltageDist(rng);
        double c = currentDist(rng);
        double r = (c > 0.1) ? v / c : 100.0; // R = U/I
        double p = v * c; // P = U*I
        
        if (voltage) voltage->writeValue(v);
        if (current) current->writeValue(c);
        if (resistance) resistance->writeValue(r);
        if (power) power->writeValue(p);
        
        std::cout << "Мультиметр: Напряжение = " << v << " В, Ток = " << c 
                  << " А, Сопротивление = " << r << " Ом, Мощность = " << p << " Вт" << std::endl;
    }
};

// ============================== КЛАСС СТАНКА ==============================
class Machine : public OPCUADevice {
private:
    OPCUAComponentVariable* flywheelRPM;
    OPCUAComponentVariable* power;
    OPCUAComponentVariable* voltage;
    OPCUAComponentVariable* energyConsumption;
    std::mt19937 rng;
    double baseRPM;
    
public:
    Machine(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 200, "Machine", "Станок", 
                     "Промышленный станок с электроприводом"),
          rng(std::random_device{}()),
          baseRPM(1500.0),
          flywheelRPM(nullptr),
          power(nullptr),
          voltage(nullptr),
          energyConsumption(nullptr) {
        
        // Создаем компоненты станка
        auto flywheelRPMVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 201, "FlywheelRPM", "Обороты маховика", 
            "Скорость вращения маховика (об/мин)", baseRPM, nodeId);
        
        auto powerVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 202, "Power", "Мощность", 
            "Потребляемая мощность (кВт)", 7.5, nodeId);
        
        auto voltageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 203, "Voltage", "Напряжение", 
            "Рабочее напряжение (Вольты)", 380.0, nodeId);
        
        auto energyVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 204, "EnergyConsumption", "Потребление энергии", 
            "Потребление энергии (кВт·ч)", 56.3, nodeId);
        
        flywheelRPM = flywheelRPMVar.get();
        power = powerVar.get();
        voltage = voltageVar.get();
        energyConsumption = energyVar.get();
        
        addComponent(std::move(flywheelRPMVar));
        addComponent(std::move(powerVar));
        addComponent(std::move(voltageVar));
        addComponent(std::move(energyVar));
    }
    
    void updateValues() override {
        // Симуляция работы станка с небольшими флуктуациями
        std::normal_distribution<double> rpmNoise(0.0, 10.0);
        std::normal_distribution<double> powerNoise(0.0, 0.1);
        
        double rpm = std::max(0.0, baseRPM + rpmNoise(rng));
        double pwr = 7.5 + powerNoise(rng);
        double volt = 380.0 + (rng() % 20 - 10); // ±10V
        double energy = 56.3 + (pwr * 0.001); // Увеличиваем пропорционально мощности
        
        if (flywheelRPM) flywheelRPM->writeValue(rpm);
        if (power) power->writeValue(pwr);
        if (voltage) voltage->writeValue(volt);
        if (energyConsumption) energyConsumption->writeValue(energy);
        
        std::cout << "Станок: Обороты = " << rpm << " об/мин, Мощность = " << pwr 
                  << " кВт, Напряжение = " << volt << " В, Энергия = " << energy << " кВт·ч" << std::endl;
    }
    
    void setBaseRPM(double rpm) {
        baseRPM = rpm;
    }
};

// ============================== КЛАСС КОМПЬЮТЕРА ==============================
class Computer : public OPCUADevice {
private:
    OPCUAComponentVariable* fan1;
    OPCUAComponentVariable* fan2;
    OPCUAComponentVariable* fan3;
    OPCUAComponentVariable* cpuLoad;
    OPCUAComponentVariable* gpuLoad;
    OPCUAComponentVariable* ramUsage;
    std::mt19937 rng;
    
public:
    Computer(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 300, "Computer", "Компьютер", 
                     "Системный блок с мониторингом параметров"),
          rng(std::random_device{}()),
          fan1(nullptr),
          fan2(nullptr),
          fan3(nullptr),
          cpuLoad(nullptr),
          gpuLoad(nullptr),
          ramUsage(nullptr) {
        
        // Создаем компоненты компьютера
        auto fan1Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 301, "Fan1", "Вентилятор 1", 
            "Скорость вентилятора ЦП (об/мин)", 1200.0, nodeId);
        
        auto fan2Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 302, "Fan2", "Вентилятор 2", 
            "Скорость вентилятора корпуса (об/мин)", 800.0, nodeId);
        
        auto fan3Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 303, "Fan3", "Вентилятор 3", 
            "Скорость вентилятора блока питания (об/мин)", 1000.0, nodeId);
        
        auto cpuLoadVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 304, "CPULoad", "Загрузка ЦП", 
            "Загрузка центрального процессора (%)", 30.0, nodeId);
        
        auto gpuLoadVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 305, "GPULoad", "Загрузка ГП", 
            "Загрузка графического процессора (%)", 25.0, nodeId);
        
        auto ramUsageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 306, "RAMUsage", "Использование ОЗУ", 
            "Использование оперативной памяти (%)", 45.0, nodeId);
        
        fan1 = fan1Var.get();
        fan2 = fan2Var.get();
        fan3 = fan3Var.get();
        cpuLoad = cpuLoadVar.get();
        gpuLoad = gpuLoadVar.get();
        ramUsage = ramUsageVar.get();
        
        addComponent(std::move(fan1Var));
        addComponent(std::move(fan2Var));
        addComponent(std::move(fan3Var));
        addComponent(std::move(cpuLoadVar));
        addComponent(std::move(gpuLoadVar));
        addComponent(std::move(ramUsageVar));
    }
    
    void updateValues() override {
        // Симуляция параметров компьютера
        std::uniform_real_distribution<double> fanDist(800.0, 1800.0);
        std::uniform_real_distribution<double> loadDist(20.0, 80.0);
        std::uniform_real_distribution<double> ramDist(30.0, 70.0);
        
        double f1 = fanDist(rng);
        double f2 = fanDist(rng);
        double f3 = fanDist(rng);
        double cpu = loadDist(rng);
        double gpu = loadDist(rng);
        double ram = ramDist(rng);
        
        // Вентиляторы реагируют на загрузку
        f1 = 1000 + cpu * 10;
        f2 = 800 + (cpu + gpu) * 5;
        f3 = 900 + (cpu * 0.7 + gpu * 0.3) * 8;
        
        if (fan1) fan1->writeValue(f1);
        if (fan2) fan2->writeValue(f2);
        if (fan3) fan3->writeValue(f3);
        if (cpuLoad) cpuLoad->writeValue(cpu);
        if (gpuLoad) gpuLoad->writeValue(gpu);
        if (ramUsage) ramUsage->writeValue(ram);
        
        std::cout << "Компьютер: Вентиляторы = [" << f1 << ", " << f2 << ", " << f3 
                  << "] об/мин, ЦП = " << cpu << "%, ГП = " << gpu 
                  << "%, ОЗУ = " << ram << "%" << std::endl;
    }
};

// ============================== КЛАСС СЕРВЕРА OPC UA ==============================
class OPCUAServer {
private:
    UA_Server* server;
    UA_UInt16 namespaceIndex;
    std::atomic<bool> running;
    std::unique_ptr<Multimeter> multimeter;
    std::unique_ptr<Machine> machine;
    std::unique_ptr<Computer> computer;
    
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
        namespaceIndex = UA_Server_addNamespace(server, "EquipmentNamespace");
        
        // Создаем устройства
        multimeter = std::make_unique<Multimeter>(server, namespaceIndex);
        multimeter->initialize();
        
        machine = std::make_unique<Machine>(server, namespaceIndex);
        machine->initialize();
        
        computer = std::make_unique<Computer>(server, namespaceIndex);
        computer->initialize();
        
        return true;
    }
    
    bool start() {
        std::cout << "\n===========================================" << std::endl;
        std::cout << "OPC UA Server запущен на opc.tcp://localhost:4840" << std::endl;
        std::cout << "===========================================" << std::endl;
        std::cout << "\nСтруктура устройств и переменных:" << std::endl;
        std::cout << "\n1. Мультиметр (ID: ns=" << namespaceIndex << ";i=100)" << std::endl;
        std::cout << "   ├── Напряжение (ID: ns=" << namespaceIndex << ";i=101)" << std::endl;
        std::cout << "   ├── Сила тока (ID: ns=" << namespaceIndex << ";i=102)" << std::endl;
        std::cout << "   ├── Сопротивление (ID: ns=" << namespaceIndex << ";i=103)" << std::endl;
        std::cout << "   └── Мощность (ID: ns=" << namespaceIndex << ";i=104)" << std::endl;
        
        std::cout << "\n2. Станок (ID: ns=" << namespaceIndex << ";i=200)" << std::endl;
        std::cout << "   ├── Обороты маховика (ID: ns=" << namespaceIndex << ";i=201)" << std::endl;
        std::cout << "   ├── Мощность (ID: ns=" << namespaceIndex << ";i=202)" << std::endl;
        std::cout << "   ├── Напряжение (ID: ns=" << namespaceIndex << ";i=203)" << std::endl;
        std::cout << "   └── Потребление энергии (ID: ns=" << namespaceIndex << ";i=204)" << std::endl;
        
        std::cout << "\n3. Компьютер (ID: ns=" << namespaceIndex << ";i=300)" << std::endl;
        std::cout << "   ├── Вентилятор 1 (ID: ns=" << namespaceIndex << ";i=301)" << std::endl;
        std::cout << "   ├── Вентилятор 2 (ID: ns=" << namespaceIndex << ";i=302)" << std::endl;
        std::cout << "   ├── Вентилятор 3 (ID: ns=" << namespaceIndex << ";i=303)" << std::endl;
        std::cout << "   ├── Загрузка ЦП (ID: ns=" << namespaceIndex << ";i=304)" << std::endl;
        std::cout << "   ├── Загрузка ГП (ID: ns=" << namespaceIndex << ";i=305)" << std::endl;
        std::cout << "   └── Использование ОЗУ (ID: ns=" << namespaceIndex << ";i=306)" << std::endl;
        std::cout << "\n===========================================" << std::endl;
        std::cout << "Для остановки сервера нажмите Ctrl+C" << std::endl;
        std::cout << "===========================================\n" << std::endl;
        
        // Запускаем сервер
        UA_StatusCode status = UA_Server_run_startup(server);
        if (status != UA_STATUSCODE_GOOD) {
            std::cerr << "Failed to start server: " << UA_StatusCode_name(status) << std::endl;
            return false;
        }
        
        return true;
    }
    
    void run() {
        int counter = 0;
        while (running) {
            // Очищаем экран для красивого вывода (только для Windows)
            clearConsole();
            
            std::cout << "===========================================" << std::endl;
            std::cout << "ЦИКЛ ОБНОВЛЕНИЯ: " << ++counter << std::endl;
            std::cout << "===========================================" << std::endl;
            
            // Обновляем значения всех устройств
            if (multimeter) {
                multimeter->updateValues();
            }
            
            if (machine) {
                machine->updateValues();
            }
            
            if (computer) {
                computer->updateValues();
            }
            
            std::cout << "===========================================" << std::endl;
            
            // Обрабатываем сетевые события
            UA_Server_run_iterate(server, false);
            
            // Пауза между обновлениями
            std::this_thread::sleep_for(std::chrono::milliseconds(330));
        }
    }
    
    void stop() {
        if (!running) return; // Уже остановлен
        
        running = false;
        
        if (server) {
            std::cout << "\nОстановка сервера..." << std::endl;
            
            // ВАЖНО: Сначала очищаем все узлы, которые ссылаются на сервер
            computer.reset();
            machine.reset();
            multimeter.reset();
            
            // Затем останавливаем и удаляем сервер
            UA_Server_run_shutdown(server);
            UA_Server_delete(server);
            server = nullptr;
            
            std::cout << "Сервер остановлен." << std::endl;
        }
    }
    
private:
    void initConsole() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    }
    
    void clearConsole() {
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }
};

// ============================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ ОБРАБОТКИ СИГНАЛОВ ==============================
std::atomic<bool> globalRunning(true);

void signalHandler(int signal) {
    (void)signal;
    std::cout << "\nПолучен сигнал остановки, останавливаю сервер..." << std::endl;
    globalRunning = false;
}

// ============================== ТОЧКА ВХОДА ==============================
int main() {
    // Инициализация консоли
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    
    std::cout << "Запуск OPC UA сервера..." << std::endl;
    
    // Устанавливаем обработчики сигналов
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    try {
        // Создаем и запускаем сервер
        OPCUAServer server;
        
        if (!server.initialize()) {
            std::cerr << "Ошибка инициализации сервера!" << std::endl;
            return 1;
        }
        
        if (!server.start()) {
            std::cerr << "Ошибка запуска сервера!" << std::endl;
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
        std::cerr << "Исключение: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Сервер завершил работу успешно." << std::endl;
    return 0;
}