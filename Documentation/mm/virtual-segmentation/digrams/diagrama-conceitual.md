```mermaid
---
config:
    layout: elk
---
flowchart TD
    subgraph U["Espaço do Usuário"]
        P["Processo de Usuário"]
    end
    subgraph SV["Módulo de Segmentação Virtual"]
        M["Monitor de Acesso à Memória"]
        G["Gerenciador de Segmentos"]
        PF["Prefetcher Assíncrono"]
    end
    subgraph S["Subsistemas do Kernel"]
        MMU["MMU / Page Table Manager"]
        PC["Page Cache"]
        SW["Swap Manager"]
    end
    subgraph K["Kernel Linux"]
        SV
        S
    end
    subgraph O["Observabilidade e Métricas"]
        PROC["/proc e sysfs"]
        PERF["Perf / eBPF Hooks"]
    end


    P -- Acesso à página --> M
    M -- Detecta padrões --> G
    G -- Define ou divide segmentos --> G
    G -- Notifica prefetch --> PF
    PF -- Carrega páginas correlatas --> PC
    PF -- Atualiza mapeamento --> MMU
    G -- Exporta estado --> PROC
    M -- Envia métricas --> PERF
    
    
    P:::user
    M:::kernel
    G:::kernel
    PF:::kernel
    MMU:::kernel
    PC:::kernel
    SW:::kernel
    PROC:::observe
    PERF:::observe
    
    
    classDef user fill:#d2eaff,stroke:#1e64c8,color:#000
    classDef kernel fill:#fff5cc,stroke:#c6a600,color:#000y
