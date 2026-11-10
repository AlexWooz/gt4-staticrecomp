# GT4 Recomp — Task: Cloud Saves & Cloud Replays via Google Drive

**Status:** Proposta para o roadmap / implementação futura  
**Tipo:** Feature (launcher + integração com o jogo + plataformas suportadas)  
**Prioridade:** A definir  
**Responsáveis:** Mantenedor (configuração Google Cloud) e agente de programação (análise, implementação, testes e documentação)

> **Instrução ao agente:** registrar este documento como uma task/issue de roadmap no repositório do GT4 Recomp. Antes de implementar, estudar a arquitetura real do projeto e os caminhos de leitura/gravação de saves e replays. Este documento descreve o comportamento desejado e os critérios de aceitação, **não uma solução em código**.

## 1. Objetivo

Criar um sistema opcional de **sincronização de saves e replays na nuvem**, integrado ao GT4 Recomp, para permitir que um jogador continue o mesmo progresso em diferentes dispositivos (PC, Android e futuras plataformas), usando **a própria conta e o próprio armazenamento Google Drive**.

- Não depender de servidor próprio, VPS, banco de dados remoto ou armazenamento pago do projeto para o tráfego normal de sincronização.
- Usar a **Google Drive API oficial** com o espaço oculto de dados do aplicativo (**`appDataFolder`**), acessado pelo escopo mínimo **`drive.appdata`**.
- O conteúdo deve ficar invisível no Meu Drive convencional e acessível somente à integração autorizada do aplicativo.
- Sincronizar automaticamente, com uma experiência semelhante à da Steam Cloud, mas **sem sobrescrever silenciosamente saves divergentes**.
- A funcionalidade deve ser **opt-in**, não obrigatória para jogar, e preservar completamente o funcionamento offline.

## 2. Experiência desejada para o jogador

### 2.1 Conectar a conta

1. O launcher oferece **Configurações > Cloud Saves / Cloud Replays > Conectar Google Drive**.
2. O jogador autoriza o aplicativo pelo fluxo oficial do Google; **o launcher nunca solicita nem armazena a senha da conta Google**.
3. O launcher informa que os dados serão guardados no espaço oculto da conta do jogador e consumirão a cota pessoal de armazenamento dele.
4. A partir daí, sincronização de saves e replays habilitados ocorre automaticamente, sem pedir login a cada execução enquanto a autorização continuar válida.
5. O jogador pode **desativar a sincronização**, **desconectar a conta**, consultar o **status da última sincronização** e acessar **restauração/exportação manual**.
6. Falha de autenticação, acesso revogado ou cota cheia não podem impedir o uso offline nem apagar arquivos locais.

### 2.2 Início de jogo

1. Ao abrir o launcher, consultar o estado remoto e o estado local, incluindo a versão do jogo/perfil pertinente.
2. Se somente a nuvem tiver progresso mais recente **sem conflito**, trazer o progresso antes de iniciar o jogo.
3. Se somente o dispositivo tiver alterações ainda não sincronizadas **sem conflito**, enviá-las.
4. Se houver divergência ou dúvida, **preservar ambas as cópias** e abrir uma tela de resolução de conflito antes da escolha do save a carregar.
5. Exibir informações compreensíveis: origem/dispositivo, data e hora, progresso identificado quando viável, estado de sincronização e opção de manter/recuperar cada versão.
6. Não misturar acidentalmente saves incompatíveis de **GT4 Retail USA**, **GT4 Spec II**, ou de outros perfis/slots que o projeto suporte.

### 2.3 Sincronização durante o jogo (requisito essencial)

- **Não depender de fechar o jogo.** Cada save concluído dentro do GT4 deve gerar uma solicitação automática de sincronização assim que os dados estiverem em um estado íntegro e seguro para leitura.
- O agente deve **investigar o sistema real de saves do jogo e a integração com o launcher/recompilador**, identificando os momentos confiáveis de conclusão de gravação. Não presumir que observar a alteração de um arquivo seja suficiente.
- Nunca capturar arquivos parcialmente escritos, bloquear o salvamento, causar stutter relevante ou sobrescrever dados que o jogo ainda esteja modificando.
- A operação de rede deve ser assíncrona: se a internet estiver lenta ou indisponível, o jogo continua normalmente e o envio fica pendente para nova tentativa.
- Na saída do jogo, verificar se restaram alterações não sincronizadas, sem fazer disso o único momento de upload.
- Mostrar um indicador discreto no launcher: **Sincronizado**, **Sincronizando**, **Pendente/offline**, **Conflito** ou **Erro**, conforme o caso.

### 2.4 Replays na nuvem

- Investigar onde e como o GT4 Recomp cria, salva, importa e carrega replays em cada versão/plataforma.
- Sincronizar automaticamente os replays concluídos e compatíveis, sem subir o mesmo replay repetidamente.
- Disponibilizar uma lista de replays na nuvem, com opção de baixar/restaurar quando necessário.
- Gerenciar o consumo de armazenamento: replays podem ser maiores que saves. Permitir **habilitar/desabilitar Cloud Replays separadamente** de Cloud Saves; mostrar uso estimado e, quando necessário, oferecer limpeza seletiva mediante confirmação.
- **Compartilhamento público por link não faz parte desta task**, pois o `appDataFolder` não permite compartilhar seus arquivos. Isso poderá ser uma feature futura com outra modalidade de armazenamento/permissão.

## 3. Conflitos: comportamento semelhante à Steam Cloud

**Regra central: nunca usar apenas “o arquivo com data mais recente vence” em situações de divergência.** O sistema deve distinguir uma continuação legítima de um progresso de duas sessões independentes.

### Cenários obrigatórios

1. **PC A joga e salva; PC B abre depois:** PC B recebe a versão sincronizada, sem aviso desnecessário.
2. **PC e Android abertos ao mesmo tempo (ou dois PCs):** detectar sessões concorrentes quando possível. Se elas seguirem caminhos de progresso independentes, gerar e preservar **cópias/snapshots distintos**, sem sobrescrever uma pela outra.
3. **Duas sessões salvam quase simultaneamente:** preservar ambas as versões, mesmo que a segunda sincronização chegue por último.
4. **Dispositivo jogou offline e outro jogou online:** ao reconectar, identificar a divergência e criar um conflito recuperável.
5. **Falha ou interrupção no upload:** a última cópia válida continua intacta; o envio pode ser retomado/repetido sem corromper ou duplicar indevidamente os dados.
6. **Conta desconectada e reconectada:** não substituir automaticamente progresso local que tenha divergido nesse período.

### Interface para resolução de conflito

- Exibir um aviso claro no launcher, por exemplo: **“Encontramos dois saves diferentes. Nenhum foi apagado.”**
- Mostrar cartões para **Save local** e **Save na nuvem / outra sessão** com **data/hora, dispositivo, versão do GT4, estado e detalhes disponíveis**.
- Oferecer ações equivalentes a **Usar esta versão**, **Preservar ambas**, **Visualizar versões anteriores** e **Cancelar / decidir depois**, de acordo com as possibilidades reais do jogo.
- **Antes de ativar a versão escolhida, manter backup da alternativa** e explicar que a seleção define qual progresso será carregado, não que o outro será apagado imediatamente.
- Se houver sessão concorrente detectada, avisar ao usuário que continuar jogando nos dois dispositivos pode gerar versões separadas.
- Na próxima abertura do launcher, avisar que houve criação de uma cópia por conflito e permitir gerenciar/restaurar essas versões.

**Observação de arquitetura:** como não haverá servidor próprio central para coordenar sessões, a detecção de simultaneidade pode ser de melhor esforço, especialmente com dispositivos offline. **A garantia exigida é preservar saves divergentes e detectar conflitos quando os dispositivos voltarem a sincronizar**, não prometer presença instantânea e infalível entre dispositivos. O agente deve validar essa garantia diante de uploads concorrentes reais.

## 4. Integridade, backups e segurança

- Preservar sempre uma cópia local íntegra antes de aplicar uma versão vinda da nuvem.
- Manter histórico limitado de versões recuperáveis dos saves, com política clara de retenção e possibilidade de restauração manual.
- Relacionar os dados ao jogo/edição, perfil/slot, dispositivo e histórico de sincronização para evitar misturas e identificar divergências.
- Verificar integridade após upload/download e tratar arquivos ausentes ou corrompidos sem eliminar a última cópia válida.
- Nunca excluir dados definitivamente sem confirmação explícita; explicar que arquivos no `appDataFolder` não têm lixeira normal do Drive.
- Garantir que o primeiro uso, a troca de conta Google e a recuperação depois de reinstalação não destruam progresso local.
- Usar autenticação OAuth oficial, com privilégios mínimos, credenciais de usuário protegidas no sistema e possibilidade de revogar acesso. **Não exigir acesso completo ao Drive.**
- Como o cliente é distribuído ao público, **não colocar chaves privadas, tokens de usuários ou segredos operacionais no repositório**. O agente deve tratar credenciais de apps desktop/mobile como clientes públicos, conforme documentação oficial.

## 5. Minha parte — passo a passo no Google Cloud

- [ ] Criar um projeto próprio para **GT4 Recomp** no [Google Cloud Console](https://console.cloud.google.com/).
- [ ] Ativar a **Google Drive API**.
- [ ] Configurar a tela de consentimento OAuth / Google Auth Platform: nome exibido, contato, público externo e demais dados exigidos.
- [ ] Configurar a permissão mínima **`https://www.googleapis.com/auth/drive.appdata`**; não solicitar acesso amplo aos arquivos pessoais.
- [ ] Criar um **OAuth Client ID do tipo Desktop** para o launcher no PC (e desktop em outras plataformas, quando aplicável).
- [ ] Criar um **OAuth Client ID do tipo Android** quando o cliente Android estiver pronto, usando o identificador do pacote e os certificados/assinaturas exigidos pelo Google.
- [ ] Disponibilizar os **Client IDs e configurações públicas necessárias** ao agente por meio seguro, sem compartilhar tokens pessoais ou segredos privados. Se houver dados confidenciais de desenvolvimento, não incluí-los em commits públicos.
- [ ] Adicionar contas de teste para validação inicial e fazer testes de autorização nos dispositivos reais.
- [ ] Antes de distribuir publicamente, configurar o status de publicação OAuth adequado e conferir eventuais requisitos de **verificação de marca, domínio, site/política de privacidade e consentimento**. Não deixar a versão pública permanentemente em modo Testing.
- [ ] Conferir quotas e faturamento no projeto, evitar ativar cobrança desnecessária e revisar as condições atuais de uso da API antes de liberar para muitos usuários.

**Entregável desta etapa:** acesso ao Google Drive API preparado e credenciais OAuth de cliente disponibilizadas ao agente, **não** uma infraestrutura de backend.

## 6. Parte do agente de programação — plano de execução

> **Sem prescrição de código, linguagens, bibliotecas ou arquivos específicos.** Analisar o repositório antes de decidir a implementação.

### Fase A — Mapeamento do sistema atual

- [ ] Identificar no código a arquitetura do launcher, o ciclo de vida do jogo e as diferenças entre os targets desktop/Android.
- [ ] Investigar a fundo **como saves são gravados e lidos** no GT4 Recomp: memory card virtual/arquivos, perfis, slots, checkpoints reais de conclusão, travas e possíveis operações assíncronas.
- [ ] Investigar **como replays são salvos, localizados, nomeados, restaurados e carregados**, incluindo compatibilidade entre Retail USA e Spec II.
- [ ] Mapear situações em que saves/replays possam estar parcialmente escritos ou sendo utilizados pelo jogo.
- [ ] Apresentar uma proposta de integração no projeto existente que minimize mudanças desnecessárias e preserve o funcionamento offline.

### Fase B — Autenticação e preferências

- [ ] Integrar o login/autorização oficial do Google nos clientes suportados.
- [ ] Permitir conectar, desconectar, reautorizar e visualizar a conta conectada.
- [ ] Criar controles separados para **Cloud Saves** e **Cloud Replays**.
- [ ] Tratar falta de rede, autorização revogada, conta trocada e problemas de cota sem bloquear o jogo.

### Fase C — Sincronização funcional

- [ ] Criar a lógica de sincronização entre estado local e `appDataFolder`, respeitando separação por versão, perfil e tipo de dado.
- [ ] Sincronizar antes de iniciar o jogo e **imediatamente após cada gravação concluída** durante a partida, sem esperar o jogo fechar.
- [ ] Sincronizar novos replays concluídos, com deduplicação e controle de uso.
- [ ] Implementar tratamento de uploads pendentes, erros transitórios, reconexão e checagem de integridade.
- [ ] Evitar sincronizações repetitivas e consultas excessivas à API; priorizar baixo consumo de banda e cotas.

### Fase D — Conflitos e interface

- [ ] Implementar proteção contra divergências e escritas concorrentes; **nunca adotar sobrescrita cega do último upload**.
- [ ] Manter cópias recuperáveis quando ocorrer conflito, inclusive entre PC/Android e dois PCs.
- [ ] Criar a experiência de resolução de conflitos no launcher, com dados de origem, datas, versões, escolha do progresso e restauração de backups.
- [ ] Exibir notificações discretas quando uma cópia de conflito for criada e solicitar decisão na próxima abertura quando necessário.
- [ ] Implementar status claro de sincronização e erros, preferencialmente localizado em **PT-BR e EN**.

### Fase E — Testes, validação e documentação

- [ ] Testar PC → Android → PC e PC A → PC B com a **mesma conta Google**.
- [ ] Testar saves durante o jogo, sem encerrar o executável, e verificar que são enviados somente após a gravação íntegra.
- [ ] Testar Retail USA e Spec II separadamente, além dos perfis/slots pertinentes.
- [ ] Testar sessões simultâneas, uploads quase simultâneos e edição offline em dois dispositivos.
- [ ] Testar falta de internet, reconexão, cota cheia, tokens expirados/revogados, instalação limpa e troca de conta.
- [ ] Testar restauração manual e integridade de saves/replays baixados.
- [ ] Documentar configurações, requisitos, riscos conhecidos e fluxo de recuperação sem incluir informações secretas.

## 7. Critérios de aceitação / definição de pronto

- [ ] Jogador conecta o Google Drive uma vez e pode ativar/desativar a função no launcher.
- [ ] Save criado no PC aparece no Android na próxima sincronização; o inverso também funciona.
- [ ] **Um save realizado durante a partida é enviado automaticamente assim que terminar de ser gravado**, sem precisar fechar o jogo.
- [ ] O jogo continua funcionando normalmente sem internet; progresso pendente sincroniza depois.
- [ ] Replays podem ser sincronizados e recuperados quando a opção específica estiver ativada.
- [ ] Saves divergentes, inclusive após sessões simultâneas ou offline, são mantidos como cópias independentes; nenhuma versão é perdida por substituição silenciosa.
- [ ] O launcher identifica conflitos e informa claramente qual versão está sendo usada, oferecendo seleção e recuperação.
- [ ] Retail USA, Spec II e diferentes perfis não se contaminam.
- [ ] O sistema permanece **sem servidor de armazenamento próprio**, usando o Drive do jogador, e opera dentro das cotas e limitações definidas para o projeto.
- [ ] Autenticação segue a documentação do Google, solicita apenas acesso necessário e não expõe dados pessoais/credenciais.
- [ ] Testes funcionais e de integridade documentados em pelo menos dois dispositivos.

## 8. Fora de escopo desta task

- Servidor próprio de saves/replays, VPS, infraestrutura de multiplayer ou banco de dados central.
- Compartilhamento público de replays por link ou biblioteca comunitária global.
- Sincronização de arquivos arbitrários da conta Google ou permissões de acesso completo ao Drive.
- Mesclagem automática de duas carreiras divergentes em uma única carreira.
- Alterar o comportamento de quando o próprio GT4 decide gravar o progresso; a tarefa captura e sincroniza **saves efetivamente concluídos**.

## 9. Referências oficiais para o agente

- [Google Drive — Store application-specific data (`appDataFolder`)](https://developers.google.com/workspace/drive/api/guides/appdata)
- [Google OAuth 2.0 — Desktop Apps / PKCE](https://developers.google.com/identity/protocols/oauth2/native-app)
- [Google Drive API — Usage limits](https://developers.google.com/workspace/drive/api/guides/limits)
- [Google Cloud — OAuth app verification](https://support.google.com/cloud/answer/13463073)

---

**Nota final:** o objetivo é uma experiência de **Cloud Saves semelhante à Steam Cloud**, com sincronização no momento do save e tratamento seguro de conflitos. A arquitetura concreta deve ser definida **após auditoria do código real do GT4 Recomp**. Esta task é uma especificação funcional para implementação futura, não uma solicitação de começar a desenvolver agora.
