// Caixa 3D do Boi (ESP32 + Relé + Amplificador + 2 Alto-Falantes 2")
// Para gerar o arquivo STL para impressão 3D, instale o software grátis OpenSCAD (openscad.org),
// cole este código e aperte F6 para renderizar, depois F7 para exportar para STL.

$fn = 60; // Suavização dos círculos

// Dimensões internas da caixa (Ajustado para caber os componentes)
width = 140;   
length = 90;   
height = 60;   
wall = 2; // Espessura da parede da caixa de plástico

// Variáveis do Alto-Falante (2 polegadas = ~50mm)
speaker_dia = 50; 
speaker_spacing = 65; 

module box() {
    difference() {
        // Corpo principal externo
        cube([width + wall*2, length + wall*2, height + wall], center=true);
        
        // Oco interno
        translate([0, 0, wall])
            cube([width, length, height], center=true);
            
        // Furos redondos frontais para os 2 Alto-falantes de 2" (50mm)
        translate([-speaker_spacing/2, (length/2 + wall/2), 0])
            rotate([90, 0, 0])
            cylinder(h=wall*3, d=speaker_dia, center=true);
            
        translate([speaker_spacing/2, (length/2 + wall/2), 0])
            rotate([90, 0, 0])
            cylinder(h=wall*3, d=speaker_dia, center=true);
            
        // Rasgo traseiro para passar os fios da Fita Touch, Botão e Fonte de Energia
        translate([0, -(length/2 + wall/2), -height/4])
            cube([30, wall*3, 10], center=true);
            
        // Rasgos superiores para Ventilação (Calor do Relé e Amplificador)
        for(i = [-30 : 15 : 30]) {
            translate([i, 0, height/2 + wall/2])
                cube([5, 60, wall*3], center=true);
        }
    }
}

module lid() {
    // Tampa de encaixe simples
    translate([0, 0, height/2 + wall + 20]) { // A tampa é desenhada acima da caixa
        union() {
            // Topo da tampa
            cube([width + wall*2, length + wall*2, wall], center=true);
            // Aba interna para encaixar por dentro da caixa
            translate([0, 0, -wall])
                difference() {
                    cube([width - 0.5, length - 0.5, wall], center=true);
                    cube([width - 4, length - 4, wall*2], center=true); // oca a aba
                }
        }
    }
}

// Renderiza a caixa e a tampa (Para impressão, comente a tampa ou a caixa para gerar dois STLs separados)
box();
lid();
