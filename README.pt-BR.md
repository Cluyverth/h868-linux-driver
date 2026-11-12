# h868-linux-driver

![Linguagem: C](https://img.shields.io/badge/linguagem-C-00599C)
![Plataforma: Linux](https://img.shields.io/badge/plataforma-Linux-FCC624)
![Subsistema: DVB](https://img.shields.io/badge/subsistema-DVB-0078D4)
![Licença: GPL-2.0-or-later](https://img.shields.io/badge/licen%C3%A7a-GPL--2.0--or--later-blue)
![Estado: experimental](https://img.shields.io/badge/estado-experimental-orange)

[English](README.md)

Driver experimental do kernel Linux para recepção de TV digital terrestre ISDB-T com o receptor USB **`07ca:0868`**.

Na configuração testada, o receptor não era reconhecido por um driver do Linux. Este projeto fornece o módulo `h868`, que inicializa o aparelho e o disponibiliza pela API DVB do Linux.

> **Aviso:** este driver executa código no kernel e ainda é experimental. Uma falha pode travar o sistema. Salve seu trabalho antes de testá-lo.

## Hardware e compatibilidade

- Identificador USB suportado: **`07ca:0868`**. Confira o aparelho com `lsusb -d 07ca:0868`. O nome comercial, sozinho, não garante que seja o mesmo hardware.
- Sintonizador: TDA18271, usando o driver já existente no kernel Linux.
- Demodulador: o modelo exato ainda não foi confirmado.
- Variantes da ponte USB com `bcdDevice=0` são recusadas, pois usam um protocolo antigo que não foi validado.

A recepção foi verificada em um AVerTV Hybrid Volar HD PRO H868 (`07ca:0868`), com antena externa e kernel CachyOS `7.3.3-1-cachyos`. Outros aparelhos, kernels e distribuições ainda precisam de validação.

## Funcionalidades e limitações

O driver registra um adaptador DVB e permite sintonizar ISDB-T em banda de 6 MHz, informar a presença e a sincronização do sinal e entregar o fluxo de transporte digital. Medições experimentais relativas de intensidade e qualidade estão disponíveis pela API de estatísticas DVB. Elas não são valores calibrados em dBm ou dB. O driver também informa de forma experimental o modo de transmissão e o intervalo de guarda. A faixa de frequências aceita pelo driver é de 45 a 864 MHz, mas isso não garante recepção em toda a faixa.

Ainda não estão implementados ou validados:

- Contadores de erro e medições calibradas de intensidade e qualidade do sinal.
- Leitura dos parâmetros TMCC por camada, seleção individual de camadas ISDB-T e modo ISDB-Tsb.
- Entrada analógica, rádio e controle remoto.

Na desconexão, processos que mantiverem descritores DVB abertos podem atrasar a liberação da instância antiga. Repetir desconexões e reconexões nessas condições pode esgotar os números de adaptador disponíveis. Para evitar isso, feche os programas que usam o receptor antes de desconectá-lo.

## Instalação com DKMS

### Pré-requisitos

São necessários DKMS, `make`, o compilador e as ferramentas usados pelo kernel, além dos headers preparados da **mesma versão e variante** do kernel em uso. Os headers também precisam conter `drivers/media/tuners/tda18271.h`. O kernel precisa disponibilizar os módulos DVB e TDA18271.

No CachyOS testado, foram usados `dkms`, `linux-cachyos-headers`, `clang` e `llvm`.

### Cadastro e instalação

Na raiz deste repositório:

```sh
make -C kernel clean
sudo dkms add "$PWD/kernel"
sudo dkms install h868/0.1.0 -k "$(uname -r)"
dkms status h868/0.1.0
```

O comando `clean` remove apenas produtos de compilação locais. O `dkms add` copia os fontes para `/usr/src/h868-0.1.0`. Alterações posteriores neste repositório **não atualizam automaticamente** essa cópia. O `dkms install` compila e instala o módulo para o kernel indicado.

Remova uma instalação antiga do DKMS antes de instalar esta versão para evitar builds conflitantes do `h868`. Use `dkms status` para identificar o pacote e a versão antigos.

Conecte o receptor e confira o módulo e os dispositivos DVB:

```sh
lsmod | grep '^h868 '
ls -l /dev/dvb/adapter0/
```

O número do adaptador pode ser diferente de `0`. Se o módulo não carregar automaticamente, use `sudo modprobe h868`. O usuário precisa ter permissão para acessar os dispositivos DVB, conforme a configuração da distribuição.

### Atualizações de kernel e Secure Boot

Em distribuições com hooks DKMS, uma atualização de kernel normalmente dispara uma reconstrução, mas mudanças na API, headers incompatíveis ou ferramentas ausentes podem impedir o build. Confira a saída da atualização e `dkms status`.

Secure Boot ou lockdown podem exigir assinatura do módulo e confiança na chave. O DKMS pode gerar uma chave e um certificado MOK locais, mas isso não significa que a chave esteja cadastrada como confiável pelo firmware. Se a assinatura for obrigatória, consulte as instruções da distribuição.

### Remoção

Feche os programas que usam o receptor e desconecte-o. Para descarregar o módulo e remover do DKMS todas as versões compiladas deste driver:

```sh
sudo modprobe -r h868
sudo dkms remove h868/0.1.0 --all
```

A remoção do pacote não descarrega automaticamente um módulo em uso. Se outro módulo depender de `h868` ou ele ainda estiver ocupado, `modprobe -r` pode falhar. Não force a remoção.

## Código-fonte

- `kernel/h868_usb.c`: comunicação USB, alimentação, barramentos I²C e transporte do fluxo digital.
- `kernel/h868_frontend.c`: inicialização do demodulador e integração do frontend com a API DVB.
- `kernel/h868_metrics.h`: conversões das métricas relativas de sinal.
- `kernel/h868_ofdm.h`: decodificação do modo de transmissão e intervalo de guarda.
- `kernel/h868.h`: interface interna entre os componentes do driver.
- `kernel/Makefile` e `kernel/dkms.conf`: compilação do módulo e configuração DKMS.

Os testes das conversões podem ser executados sem o receptor:

```sh
cc -std=c11 -Wall -Wextra -Werror tests/h868_metrics_test.c -o /tmp/h868-metrics-test
/tmp/h868-metrics-test
cc -std=c11 -Wall -Wextra -Werror tests/h868_ofdm_test.c -o /tmp/h868-ofdm-test
/tmp/h868-ofdm-test
```

## Licença

O driver é distribuído sob a licença [GPL-2.0-or-later](LICENSE), conforme os identificadores SPDX nos arquivos de código.
