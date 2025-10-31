```mermaid
---
config:
  look: neo
---
sequenceDiagram
    autonumber
    
    
    participant P as Processo de Usuário
    participant PFH as Page Fault Handler
    participant M as Monitor de Acesso à Memória
    participant G as Gerenciador de Segmentos
    participant PR as Prefetcher Assíncrono
    participant MMU as MMU / Gerenciador de Páginas
    participant PC as Page Cache
    participant O as Perf / eBPF
    
    
    P->>PFH: Acesso a página não presente
    PFH->>M: Notifica evento de page fault
    M->>G: Atualiza estatísticas e padrões de acesso
    G->>G: Avalia necessidade de dividir/ajustar segmento
    G-->>PFH: Retorna segmento associado à página
    PFH->>MMU: Carrega página solicitada (sincronamente)
    MMU->>PC: Armazena página na cache
    MMU-->>P: Retorna controle (página disponível)
    Note right of G: Após o carregamento, um evento<br/>de prefetch é disparado
    G->>PR: Solicita prefetch de páginas correlatas
    PR->>MMU: Carrega páginas relacionadas (assíncrono)
    PR->>PC: Armazena páginas pré-carregadas
    PR-->>G: Atualiza status do segmento
    M->>O: Envia métricas de desempenho (Perf / eBPF)
